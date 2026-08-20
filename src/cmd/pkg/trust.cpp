#include "trust.h"

#include "encode.h"
#include "kernel/sysabi.h"
#include "kernel/traits.h"
#include "sha256.h"

namespace {

// One stanza of `known` and no second. The probe reads into a vector of its
// own: next() clears what it is given.
bool one_stanza(Str text, Str known, Vec<StanzaField> &f)
{
    StanzaReader r(text, known);
    if (r.next(f) != StanzaRead::Ok)
        return false;
    Vec<StanzaField> tail;
    return r.next(tail) == StanzaRead::End;
}

// H comes once per use.
bool thresholds_unique(const Anchor &a)
{
    for (usize i = 0; i < a.thresholds.size(); i++)
        for (usize j = i + 1; j < a.thresholds.size(); j++)
            if (a.thresholds[i].use == a.thresholds[j].use)
                return false;
    return true;
}

// A key appears once and an ed25519 one is 32 bytes; another algorithm's is
// left alone.
bool keys_usable(const Anchor &a)
{
    for (usize i = 0; i < a.keys.size(); i++) {
        for (usize j = i + 1; j < a.keys.size(); j++)
            if (a.keys[i].key == a.keys[j].key)
                return false;
        if (a.keys[i].algorithm != TRUST_ALGORITHM)
            continue;
        u8 raw[SYS_ED25519_KEY];
        Option<usize> n = base64_decode(a.keys[i].key, Span<u8>(raw));
        if (!n || n.value() != SYS_ED25519_KEY)
            return false;
    }
    return true;
}

} // namespace

bool trust_key_name(const AnchorKey &k, Span<char> out)
{
    Sha256 s;
    s.update(k.algorithm);
    s.update(Str(" "));
    s.update(k.key);
    u8 d[SHA256_SIZE];
    s.finish(d);
    return digest_write(d, out);
}

bool anchor_file_read(String text, AnchorFile &out)
{
    out.text = move(text);
    out.sigs.clear();
    if (!signed_split(out.text.str(), out.block, out.body))
        return false;

    Vec<StanzaField> f;
    if (!out.block.empty()) {
        if (!one_stanza(out.block, STANZA_SIGNATURE, f))
            return false;
        if (signature_read(f, out.sigs) != StanzaRead::Ok)
            return false;
    }

    if (!one_stanza(out.body, STANZA_ANCHOR, f))
        return false;
    if (anchor_read(f, out.rec) != StanzaRead::Ok)
        return false;

    // §3.1's rule, which §4 inherits: an unknown grammar refuses the file.
    if (out.rec.grammar != TRUST_GRAMMAR)
        return false;
    return thresholds_unique(out.rec) && keys_usable(out.rec);
}

u32 trust_threshold(const Anchor &a, Str use)
{
    for (const AnchorThreshold &t : a.thresholds)
        if (t.use == use)
            return t.count;
    return 0;
}

Task<Result<bool>> trust_meet(const Anchor &keys, Str use, const Vec<Signature> &sigs, Str body,
                              TrustVerify v)
{
    u32 need = trust_threshold(keys, use);
    if (need == 0)
        co_return false;

    u32 count = 0;
    for (const AnchorKey &k : keys.keys) {
        if (k.use != use || k.algorithm != TRUST_ALGORITHM)
            continue;
        u8 raw[SYS_ED25519_KEY];
        Option<usize> len = base64_decode(k.key, Span<u8>(raw));
        if (!len || len.value() != SYS_ED25519_KEY)
            continue;
        char name[DIGEST_TEXT];
        if (!trust_key_name(k, Span<char>(name)))
            continue;

        // The break is the rule: one key, one signature.
        for (const Signature &s : sigs) {
            if (s.algorithm != TRUST_ALGORITHM || s.key_name != Str(name, sizeof(name)))
                continue;
            u8 sig[SYS_ED25519_SIG];
            Option<usize> n = base64_decode(s.signature, Span<u8>(sig));
            if (!n || n.value() != SYS_ED25519_SIG)
                continue;
            Task<Result<bool>> t = v(Str(reinterpret_cast<const char *>(raw), sizeof(raw)),
                                     Str(reinterpret_cast<const char *>(sig), sizeof(sig)), body);
            if (!t)
                co_return Err(Error::NoMemory);
            Result<bool> r = co_await t;
            if (r.is_err())
                co_return Err(r.error());
            if (r.value()) {
                count++;
                break;
            }
        }
        if (count >= need)
            co_return true;
    }
    co_return false;
}

Task<Result<bool>> trust_self(const AnchorFile &a, u64 now, TrustVerify v)
{
    if (now >= a.rec.expiry)
        co_return false;
    Task<Result<bool>> t = trust_meet(a.rec, TRUST_ROOT, a.sigs, a.body, v);
    if (!t)
        co_return Err(Error::NoMemory);
    co_return co_await t;
}

Task<Result<bool>> trust_step(const AnchorFile &cur, const AnchorFile &next, u64 now, TrustVerify v)
{
    // G orders the chain; a skipped number is not itself a refusal.
    if (next.rec.version <= cur.rec.version)
        co_return false;

    Task<Result<bool>> t = trust_meet(cur.rec, TRUST_ROOT, next.sigs, next.body, v);
    if (!t)
        co_return Err(Error::NoMemory);
    Result<bool> r = co_await t;
    if (r.is_err() || !r.value())
        co_return r;

    Task<Result<bool>> u = trust_self(next, now, v);
    if (!u)
        co_return Err(Error::NoMemory);
    co_return co_await u;
}

Task<Result<usize>> trust_walk(Span<const AnchorFile> chain, u64 now, TrustVerify v)
{
    usize at = 0;
    while (at + 1 < chain.size()) {
        Task<Result<bool>> t = trust_step(chain[at], chain[at + 1], now, v);
        if (!t)
            co_return Err(Error::NoMemory);
        Result<bool> r = co_await t;
        if (r.is_err())
            co_return Err(r.error());
        if (!r.value())
            break;
        at++;
    }
    co_return at;
}

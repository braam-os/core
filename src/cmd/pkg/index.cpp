#include "index.h"

#include "db.h"
#include "dep.h"
#include "kernel/traits.h"
#include "sha256.h"
#include "trust.h"
#include "zip.h"

namespace {

// The header is the first stanza of a signed body, by position (§3).
bool header_of(Str body, IndexHeader &out)
{
    Vec<StanzaField> f;
    StanzaReader r(body, STANZA_HEADER);
    if (r.next(f) != StanzaRead::Ok)
        return false;
    return header_read(f, out) == StanzaRead::Ok;
}

// §2's block, when there is one. An absent block is an empty list.
Result<void> sigs_of(Str block, Vec<Signature> &out)
{
    if (block.empty())
        return {};
    Vec<StanzaField> f;
    if (!StanzaReader::one(block, STANZA_SIGNATURE, f))
        return Err(Error::Invalid);
    if (signature_read(f, out) != StanzaRead::Ok)
        return Err(Error::Invalid);
    return {};
}

// The stanzas after the header. §1: an unusable record is dropped, a malformed
// one takes the file down.
Result<void> packages_of(Str body, Vec<PackageStanza> &out)
{
    usize after = body.find("\n\n");
    if (after == Str::npos)
        return {};

    StanzaReader r(body.substr(after + 2), STANZA_PACKAGE);
    Vec<StanzaField> f;
    for (;;) {
        StanzaRead got = r.next(f);
        if (got == StanzaRead::End)
            return {};
        if (got == StanzaRead::Malformed)
            return Err(Error::Invalid);
        PackageStanza p;
        if (got == StanzaRead::Unusable || package_read(f, p) != StanzaRead::Ok)
            continue;
        if (!out.push(move(p)))
            return Err(Error::NoMemory);
    }
}

// The G of the last checked index. No stored index is no floor (§8.2); one
// that does not read is a refusal, since zero would erase the rollback check.
Task<Result<u64>> floor_of(PkgHost &h)
{
    Result<String> text = Err(Error::NoMemory);
    if (Task<Result<String>> t = h.load(PKG_INDEX))
        text = co_await t;
    if (text.is_err())
        co_return text.error() == Error::NotFound ? Result<u64>(u64(0)) : Err(text.error());
    if (text.value().empty())
        co_return u64(0);

    Str block, body;
    IndexHeader head;
    if (!signed_split(text.value().str(), block, body) || !header_of(body, head))
        co_return Err(Error::Invalid);
    co_return head.version;
}

} // namespace

Task<Result<String>> index_fetch_capped(PkgHost &h, Str url, u64 cap)
{
    u32 status     = 0;
    Result<i32> fd = Err(Error::NoMemory);
    if (Task<Result<i32>> t = h.open(url, status))
        fd = co_await t;
    if (fd.is_err())
        co_return Err(fd.error());

    ZipSink sink(cap);
    Error failure = Error::Invalid;
    bool ok       = false;
    if (status == 200) {
        for (;;) {
            Result<String> chunk = Err(Error::NoMemory);
            if (Task<Result<String>> t = h.read(fd.value()))
                chunk = co_await t;
            if (chunk.is_err()) {
                if (chunk.error() == Error::Closed)
                    ok = true;
                else
                    failure = chunk.error();
                break;
            }
            if (!sink.take(chunk.value().str()))
                break; // past the cap: abandoned part way
        }
    } else {
        failure = Error::NotFound;
    }

    if (Task<void> t = h.close(fd.value()))
        co_await t;
    if (!ok)
        co_return Err(failure);
    co_return move(sink.text());
}

Str repo_trim(Str url)
{
    while (url.size() > 1 && url[url.size() - 1] == '/')
        url = url.substr(0, url.size() - 1);
    return url;
}

Str index_step_name(IndexStep s)
{
    switch (s) {
    case IndexStep::Clock:
        return "clock";
    case IndexStep::Anchor:
        return "anchor";
    case IndexStep::Fetch:
        return "fetch";
    case IndexStep::Signature:
        return "signature";
    case IndexStep::Header:
        return "header";
    case IndexStep::Version:
        return "version";
    case IndexStep::Expiry:
        return "expiry";
    case IndexStep::Read:
        return "read";
    case IndexStep::Package:
        return "package";
    case IndexStep::Digest:
        return "digest";
    }
    return "?";
}

Task<Result<void>> index_check(PkgHost &h, Str repo, CheckedIndex &out, IndexStep &step)
{
    out.sigs.clear();
    out.packages.clear();
    out.unchanged = false;

    // 1. The time, once.
    step             = IndexStep::Clock;
    Result<u64> when = Err(Error::NoMemory);
    if (Task<Result<u64>> t = h.now())
        when = co_await t;
    if (when.is_err())
        co_return Err(when.error());
    out.now = when.value();

    // 2. The anchor.
    step = IndexStep::Anchor;
    AnchorFile anchor;
    Result<void> got = Err(Error::NoMemory);
    if (Task<Result<void>> t = anchor_load(h, out.now, anchor))
        got = co_await t;
    if (got.is_err())
        co_return Err(got.error());

    // 3. The index, capped.
    step = IndexStep::Fetch;
    String url;
    if (!url.append(repo) || !url.append(INDEX_LEAF))
        co_return Err(Error::NoMemory);
    Result<String> text = Err(Error::NoMemory);
    if (Task<Result<String>> t = index_fetch_capped(h, url.str(), INDEX_MAX))
        text = co_await t;
    if (text.is_err())
        co_return Err(text.error());
    out.text = move(text.value());
    if (!signed_split(out.text.str(), out.block, out.body))
        co_return Err(Error::Invalid);

    // 4. The signatures, to the anchor's index threshold.
    step = IndexStep::Signature;
    CO_TRY_VOID(sigs_of(out.block, out.sigs));
    Result<bool> met = Err(Error::NoMemory);
    if (Task<Result<bool>> t = trust_meet(anchor.rec, TRUST_INDEX, out.sigs, out.body, h))
        met = co_await t;
    if (met.is_err())
        co_return Err(met.error());
    if (!met.value())
        co_return Err(Error::Perm);

    // §3.1's two file-level rules, read with the header G and E come from.
    step = IndexStep::Header;
    if (!header_of(out.body, out.head))
        co_return Err(Error::Invalid);
    if (out.head.grammar != INDEX_GRAMMAR)
        co_return Err(Error::Unsupported);
    if (out.head.url != repo)
        co_return Err(Error::Perm);

    // 5. The version, against the highest seen before.
    step             = IndexStep::Version;
    Result<u64> seen = Err(Error::NoMemory);
    if (Task<Result<u64>> t = floor_of(h))
        seen = co_await t;
    if (seen.is_err())
        co_return Err(seen.error());
    if (out.head.version < seen.value())
        co_return Err(Error::Perm);
    out.unchanged = out.head.version == seen.value();

    // 6. The expiry, against the time from step 1.
    step = IndexStep::Expiry;
    if (out.now >= out.head.expiry)
        co_return Err(Error::Perm);

    // 7. Only now, the packages.
    step = IndexStep::Read;
    CO_TRY_VOID(packages_of(out.body, out.packages));
    co_return Result<void>();
}

Result<void> index_read(String text, CheckedIndex &out)
{
    out.sigs.clear();
    out.packages.clear();
    out.head      = IndexHeader();
    out.now       = 0;
    out.unchanged = false;

    out.text = move(text);
    if (!signed_split(out.text.str(), out.block, out.body))
        return Err(Error::Invalid);
    TRY_VOID(sigs_of(out.block, out.sigs));
    if (!header_of(out.body, out.head))
        return Err(Error::Invalid);
    if (out.head.grammar != INDEX_GRAMMAR)
        return Err(Error::Unsupported);
    return packages_of(out.body, out.packages);
}

const PackageStanza *index_find(const CheckedIndex &c, Str name)
{
    for (const PackageStanza &p : c.packages)
        if (p.name == name)
            return &p;
    return nullptr;
}

const PackageStanza *index_provides(const CheckedIndex &c, Str name)
{
    if (const PackageStanza *p = index_find(c, name))
        return p;
    for (const PackageStanza &p : c.packages) {
        Str rest = p.provides, spec;
        while (dep_next(rest, spec)) {
            Dep d;
            if (dep_parse(spec, d) != DepParse::Malformed && d.name == name)
                return &p;
        }
    }
    return nullptr;
}

bool index_vouches(const CheckedIndex &c, const u8 digest[SHA256_SIZE])
{
    for (const PackageStanza &p : c.packages) {
        bool same = true;
        for (usize i = 0; i < SHA256_SIZE; i++)
            same = same && p.digest[i] == digest[i];
        if (same)
            return true;
    }
    return false;
}

bool package_check(const PackageStanza &p, Str bytes)
{
    if (bytes.size() != p.size)
        return false;

    u8 got[SHA256_SIZE];
    sha256(Bytes(reinterpret_cast<const u8 *>(bytes.data()), bytes.size()), got);
    for (usize i = 0; i < SHA256_SIZE; i++)
        if (got[i] != p.digest[i])
            return false;
    return true;
}

Task<Result<void>> index_fetch(PkgHost &h, const CheckedIndex &c, const PackageStanza &p,
                               String &out, IndexStep &step)
{
    // §3.3: the URL is derived from the header's N, never carried by a record.
    step     = IndexStep::Package;
    Str repo = repo_trim(c.head.url);
    if (repo.empty())
        co_return Err(Error::Invalid);
    if (p.size == 0 || p.size > PACKAGE_MAX)
        co_return Err(Error::Unsupported);

    String stem, url;
    if (!pkg_stem(p.name, p.version, stem))
        co_return Err(Error::NoMemory);
    if (!url.assign(repo) || !url.push('/') || !url.append(stem.str()) || !url.append(".zip"))
        co_return Err(Error::NoMemory);

    Result<String> text = Err(Error::NoMemory);
    if (Task<Result<String>> t = index_fetch_capped(h, url.str(), p.size))
        text = co_await t;
    if (text.is_err())
        co_return Err(text.error());

    // ------------------------------ §7 step 9, and nothing has been written.
    step = IndexStep::Digest;
    if (!package_check(p, text.value().str()))
        co_return Err(Error::Perm);

    out = move(text.value());
    co_return Result<void>();
}

#include "index.h"

#include "db.h"
#include "kernel/traits.h"
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

// The body, refused past INDEX_MAX rather than truncated (§3's endless data).
Task<Result<String>> fetch_capped(PkgHost &h, Str url)
{
    u32 status     = 0;
    Result<i32> fd = Err(Error::NoMemory);
    if (Task<Result<i32>> t = h.open(url, status))
        fd = co_await t;
    if (fd.is_err())
        co_return Err(fd.error());

    ZipSink sink(INDEX_MAX);
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

} // namespace

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
    if (Task<Result<String>> t = fetch_capped(h, url.str()))
        text = co_await t;
    if (text.is_err())
        co_return Err(text.error());
    out.text = move(text.value());
    if (!signed_split(out.text.str(), out.block, out.body))
        co_return Err(Error::Invalid);

    // 4. The signatures, to the anchor's index threshold.
    step = IndexStep::Signature;
    if (!out.block.empty()) {
        Vec<StanzaField> f;
        if (!StanzaReader::one(out.block, STANZA_SIGNATURE, f))
            co_return Err(Error::Invalid);
        if (signature_read(f, out.sigs) != StanzaRead::Ok)
            co_return Err(Error::Invalid);
    }
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

    // 7. Only now, the packages. §1: an unusable record is dropped, a
    // malformed one takes the file down.
    step        = IndexStep::Read;
    usize after = out.body.find("\n\n");
    if (after != Str::npos) {
        StanzaReader r(out.body.substr(after + 2), STANZA_PACKAGE);
        Vec<StanzaField> f;
        for (;;) {
            StanzaRead got_one = r.next(f);
            if (got_one == StanzaRead::End)
                break;
            if (got_one == StanzaRead::Malformed)
                co_return Err(Error::Invalid);
            PackageStanza p;
            if (got_one == StanzaRead::Unusable || package_read(f, p) != StanzaRead::Ok)
                continue;
            if (!out.packages.push(move(p)))
                co_return Err(Error::NoMemory);
        }
    }
    co_return Result<void>();
}

const PackageStanza *index_find(const CheckedIndex &c, Str name)
{
    for (const PackageStanza &p : c.packages)
        if (p.name == name)
            return &p;
    return nullptr;
}

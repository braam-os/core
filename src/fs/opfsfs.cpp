#include "opfsfs.h"

#include "hostfs.h"
#include "kernel/host.h"
#include "kernel/traits.h"

namespace {

// A negative return from host_fs_sync is an Error, not a byte count.
Result<usize> sync_call(FsSyncOp op, u32 h, u32 ptr, u32 len, u32 off)
{
    i32 r = host_fs_sync(u32(op), h, ptr, len, off);
    if (r < 0) {
        u32 e = u32(-r);
        return Err(e && e <= u32(Error::NotEmpty) ? Error(e) : Error::Io);
    }
    return usize(r);
}

// A 32-bit host ABI, so an offset past 4 GiB is a bad call rather than a big
// file. Nothing in a browser tab is going to reach it.
Result<u32> off32(u64 v)
{
    if (v > 0xffffffffull)
        return Err(Error::Invalid);
    return u32(v);
}

} // namespace

// The reply is a run of entries, each {kind, size_lo, size_hi, mtime_lo,
// mtime_hi, name_len} in u32s followed by the name padded up to the next word.
// web/fs.js writes it.
Result<Vec<Entry>> fs_decode_entries(const char *p, usize n)
{
    Vec<Entry> out;
    usize at = 0;
    while (at + 24 <= n) {
        u32 head[6];
        __builtin_memcpy(head, p + at, sizeof(head));
        at += sizeof(head);

        usize len = head[5];
        if (at + len > n)
            return Err(Error::Io);

        Entry e;
        e.kind  = head[0] ? NodeKind::Dir : NodeKind::File;
        e.size  = u64(head[1]) | (u64(head[2]) << 32);
        e.mtime = u64(head[3]) | (u64(head[4]) << 32);
        if (!e.name.assign(Str(p + at, len)) || !out.push(move(e)))
            return Err(Error::NoMemory);
        at += (len + 3) & ~usize(3);
    }
    return move(out);
}

Task<Result<Stat>> OpfsFs::stat(Str path)
{
    FsCall c(FsOp::Stat, path, 0);
    if (!c.ok() || !c.reserve(STAT_BYTES))
        co_return Err(Error::NoMemory);
    CO_TRY_VOID(co_await c);
    if (c.req().h.buf_len < STAT_BYTES)
        co_return Err(Error::Io);

    u32 w[5];
    __builtin_memcpy(w, c.req().buf.data(), sizeof(w));
    co_return Stat{ w[0] ? NodeKind::Dir : NodeKind::File, u64(w[1]) | (u64(w[2]) << 32),
                    u64(w[3]) | (u64(w[4]) << 32) };
}

Task<Result<Vec<Entry>>> OpfsFs::list(Str path)
{
    FsCall c(FsOp::List, path, 0);
    if (!c.ok() || !c.reserve(FS_BLOCK))
        co_return Err(Error::NoMemory);

    // A directory that does not fit costs one extra round trip: the host
    // reports the room it needs and writes nothing (Concept.md §5.2 has no
    // opinion here; the alternative was a buffer big enough for anything,
    // which is a whole span the common case never uses).
    for (int attempt = 0; attempt < 2; attempt++) {
        CO_TRY_VOID(co_await c);
        HostReq &r = c.req();
        if (r.h.buf_len > 0 || r.h.result_lo == 0)
            co_return fs_decode_entries(r.buf.data(), r.h.buf_len);
        if (!c.reserve(r.h.result_lo))
            co_return Err(Error::NoMemory);
        r.done = false;
    }
    co_return Err(Error::Io);
}

Task<Result<u32>> OpfsFs::open(Str path, u32 flags)
{
    FsCall c(FsOp::Open, path, flags);
    if (!c.ok())
        co_return Err(Error::NoMemory);
    CO_TRY_VOID(co_await c);
    co_return c.req().h.result_lo;
}

Task<Result<void>> OpfsFs::mkdir(Str path)
{
    FsCall c(FsOp::Mkdir, path, 0);
    if (!c.ok())
        co_return Err(Error::NoMemory);
    co_return co_await c;
}

Task<Result<void>> OpfsFs::remove(Str path, bool all)
{
    FsCall c(FsOp::Remove, path, all ? 1u : 0u);
    if (!c.ok())
        co_return Err(Error::NoMemory);
    co_return co_await c;
}

Task<Result<void>> OpfsFs::touch(Str path)
{
    FsCall c(FsOp::Touch, path, 0);
    if (!c.ok())
        co_return Err(Error::NoMemory);
    co_return co_await c;
}

Result<usize> OpfsFs::read(u32 h, u64 off, u8 *buf, usize n)
{
    u32 at = TRY(off32(off));
    return sync_call(FsSyncOp::Read, h, host_addr(buf), u32(n), at);
}

Result<usize> OpfsFs::write(u32 h, u64 off, const u8 *buf, usize n)
{
    u32 at = TRY(off32(off));
    return sync_call(FsSyncOp::Write, h, host_addr(buf), u32(n), at);
}

Result<u64> OpfsFs::size(u32 h)
{
    return u64(TRY(sync_call(FsSyncOp::Size, h, 0, 0, 0)));
}

Result<void> OpfsFs::truncate(u32 h, u64 n)
{
    u32 to          = TRY(off32(n));
    Result<usize> r = sync_call(FsSyncOp::Truncate, h, 0, 0, to);
    if (r.is_err())
        return Err(r.error());
    return {};
}

void OpfsFs::close(u32 h)
{
    host_fs_sync(u32(FsSyncOp::Close), h, 0, 0, 0);
}

#include "hostfs.h"

#include "kernel/traits.h"

Task<Result<StorageBackend>> storage_info()
{
    // quota, usage, opfs, sync, fsaccess, persisted — 32 bytes, as web/fs.js
    // writes them.
    constexpr usize INFO_BYTES = 32;

    FsCall c(FsOp::Info, "", 0);
    if (!c.ok() || !c.reserve(INFO_BYTES))
        co_return Err(Error::NoMemory);
    CO_TRY_VOID(co_await c);
    if (c.req().h.buf_len < INFO_BYTES)
        co_return Err(Error::Io);

    const u32 *w = reinterpret_cast<const u32 *>(c.req().buf.data());
    StorageBackend b;
    b.quota     = u64(w[0]) | (u64(w[1]) << 32);
    b.usage     = u64(w[2]) | (u64(w[3]) << 32);
    b.opfs      = w[4] != 0;
    b.sync      = w[5] != 0;
    b.fsaccess  = w[6] != 0;
    b.persisted = w[7] != 0;
    co_return b;
}

Task<Result<u32>> storage_unpack(Str version)
{
    // One round trip however long it takes: the host fetches the archive,
    // reads it and writes every file, and the kernel never sees the bytes.
    // `version` rides in the slot a path would use — it is what the host
    // stamps /version with, so the stamp cannot disagree with the kernel that
    // asked for it.
    FsCall c(FsOp::Unpack, version, 0);
    if (!c.ok())
        co_return Err(Error::NoMemory);
    CO_TRY_VOID(co_await c);
    co_return c.req().h.result_lo;
}

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

Task<Result<String>> storage_bundle()
{
    FsCall c(FsOp::Bundle, "", 0);
    if (!c.ok())
        co_return Err(Error::NoMemory);

    // Two round trips at worst: the archive's size is not known until the host
    // has been asked, and a buffer big enough for every plausible one would be
    // a span the boot path does not need.
    for (int attempt = 0; attempt < 2; attempt++) {
        CO_TRY_VOID(co_await c);
        HostReq &r = c.req();
        if (r.h.buf_len > 0) {
            String out;
            if (!out.append(Str(r.buf.data(), r.h.buf_len)))
                co_return Err(Error::NoMemory);
            co_return move(out);
        }
        if (r.h.result_lo == 0)
            co_return Err(Error::NotFound);
        if (!c.reserve(r.h.result_lo))
            co_return Err(Error::NoMemory);
        r.done = false;
    }
    co_return Err(Error::Io);
}

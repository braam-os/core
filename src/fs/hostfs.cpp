#include "hostfs.h"

#include "kernel/alloc.h"
#include "kernel/host.h"
#include "kernel/traits.h"
#include "kernel/vec.h"

namespace {

// Requests whose awaiter has gone. Built on first use, so it is trivially
// destructible as a global and needs no __cxa_atexit.
Vec<FsReq *> *orphans = nullptr;

void adopt(FsReq *r)
{
    if (!orphans) {
        orphans = heap_new<Vec<FsReq *>>();
        if (!orphans) {
            r->orphan = true; // deliberately leaked: the host still holds it
            return;
        }
    }
    r->orphan = true;
    orphans->push(r); // a failed push leaks it, for the same reason
}

} // namespace

u32 host_addr(const void *p)
{
    return u32(reinterpret_cast<usize>(p));
}

FsCall::FsCall(FsOp op, Str path, u32 flags)
{
    FsReq *r = heap_new<FsReq>();
    if (!r)
        return;
    if (!r->path.assign(path)) {
        heap_delete(r);
        return;
    }

    r->h.op       = u32(op);
    r->h.flags    = flags;
    r->h.path_ptr = host_addr(r->path.data());
    r->h.path_len = u32(r->path.size());
    r->h.status   = -i32(Error::Invalid); // until the host says otherwise
    r_            = r;
}

FsCall::~FsCall()
{
    sched_unwait(&w_);
    if (!r_)
        return;
    // A record the host was never given, or has already answered, is ours to
    // free. Anything else it may still write into, so it has to outlive us.
    if (!issued_ || r_->done)
        heap_delete(r_);
    else
        adopt(r_);
}

bool FsCall::reserve(usize n)
{
    if (!r_ || !r_->buf.reserve(n))
        return false;
    r_->h.buf_ptr = host_addr(r_->buf.data());
    r_->h.buf_cap = u32(r_->buf.capacity());
    return true;
}

void FsCall::issue()
{
    r_->h.token = w_.token;
    host_fs(r_->h.op, w_.token, host_addr(&r_->h));
}

Result<void> FsCall::await_resume() const
{
    if (!r_)
        return Err(Error::NoMemory);

    // Whether the reply landed, which is what decides between freeing this
    // record and orphaning it. Only sched_cancel force-resumes a waiter, and
    // only it sets `cancelled`; a cancellation arriving after the reply leaves
    // the flag clear, and that request is answered and done with.
    r_->done = issued_ && !w_.cancelled;

    if (w_.cancelled || (w_.cancel && w_.cancel->cancelled))
        return Err(Error::Cancelled);
    if (w_.failed)
        return Err(Error::NoMemory);

    if (r_->h.status < 0) {
        u32 e = u32(-r_->h.status);
        return Err(e && e <= u32(Error::NotEmpty) ? Error(e) : Error::Io);
    }
    return {};
}

void fs_orphan_reply(u32 token)
{
    if (!orphans)
        return;
    for (usize i = 0; i < orphans->size(); i++) {
        FsReq *r = (*orphans)[i];
        if (r->h.token != token)
            continue;
        orphans->erase(i);
        heap_delete(r);
        return;
    }
}

usize fs_orphans()
{
    return orphans ? orphans->size() : 0;
}

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
        FsReq &r = c.req();
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

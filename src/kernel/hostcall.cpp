#include "hostcall.h"

#include "alloc.h"
#include "host.h"
#include "vec.h"

namespace {

// Requests whose awaiter has gone. Built on first use, so it is trivially
// destructible as a global and needs no __cxa_atexit.
Vec<HostReq *> *orphans = nullptr;

void adopt(HostReq *r)
{
    if (!orphans) {
        orphans = heap_new<Vec<HostReq *>>();
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

HostCall::HostCall(HostIface iface, u32 op, Str arg, u32 flags) : iface_(iface)
{
    HostReq *r = heap_new<HostReq>();
    if (!r)
        return;
    if (!r->arg.assign(arg)) {
        heap_delete(r);
        return;
    }

    r->h.op      = op;
    r->h.flags   = flags;
    r->h.arg_ptr = host_addr(r->arg.data());
    r->h.arg_len = u32(r->arg.size());
    r->h.status  = -i32(Error::Invalid); // until the host says otherwise
    r_           = r;
}

HostCall::~HostCall()
{
    sched_unwait(&w_);
    if (!r_)
        return;
    // A record the host was never given, or has already answered, is ours to
    // free. Anything else it may still write into, so it has to outlive us —
    // and so must any slot it was told to deposit an object in.
    if (!issued_ || r_->done)
        heap_delete(r_);
    else
        adopt(r_);
}

bool HostCall::reserve(usize n)
{
    if (!r_ || !r_->buf.reserve(n))
        return false;
    r_->h.buf_ptr = host_addr(r_->buf.data());
    r_->h.buf_cap = u32(r_->buf.capacity());
    return true;
}

bool HostCall::put(Str bytes)
{
    if (!r_ || !r_->buf.assign(bytes))
        return false;
    r_->h.buf_ptr = host_addr(r_->buf.data());
    r_->h.buf_cap = u32(r_->buf.capacity());
    r_->h.buf_len = u32(r_->buf.size());
    return true;
}

bool HostCall::put(String &&bytes)
{
    if (!r_)
        return false;
    r_->buf       = move(bytes);
    r_->h.buf_ptr = host_addr(r_->buf.data());
    r_->h.buf_cap = u32(r_->buf.capacity());
    r_->h.buf_len = u32(r_->buf.size());
    return true;
}

bool HostCall::reserve_ref()
{
    if (!r_)
        return false;
    r_->ref   = JsRef::reserve();
    r_->h.ref = r_->ref.slot();
    return r_->ref.ok();
}

void HostCall::issue()
{
    r_->h.token = w_.token;
    switch (iface_) {
    case HostIface::Fs:
        host_fs(r_->h.op, w_.token, host_addr(&r_->h));
        return;
    case HostIface::Svc:
        host_svc(r_->h.op, w_.token, host_addr(&r_->h), jsref_get(r_->h.ref));
        return;
    }
}

Result<void> HostCall::await_resume() const
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

void host_orphan_reply(u32 token)
{
    if (!orphans)
        return;
    for (usize i = 0; i < orphans->size(); i++) {
        HostReq *r = (*orphans)[i];
        if (r->h.token != token)
            continue;
        orphans->erase(i);
        heap_delete(r);
        return;
    }
}

usize host_orphans()
{
    return orphans ? orphans->size() : 0;
}

#include "xfer.h"

#include "kernel/traits.h"

Task<Result<Picked>> pick_files()
{
    SvcCall c(SvcOp::Pick);
    if (!c.ok() || !c.reserve_ref())
        co_return Err(Error::NoMemory);
    CO_TRY_VOID(co_await c);

    Picked p;
    p.count = usize(c.req().h.result_lo);
    p.set   = JsHandle(c.take_ref());
    co_return move(p);
}

Task<Result<String>> pick_name(const Picked &p, usize index)
{
    SvcCall c(SvcOp::PickName, "", u32(index));
    if (!c.ok())
        co_return Err(Error::NoMemory);
    c.set_ref(p.set.slot());

    Task<Result<String>> t = svc_blob(c);
    if (!t)
        co_return Err(Error::NoMemory);
    co_return co_await t;
}

Task<Result<String>> pick_read(const Picked &p, usize index, u64 off)
{
    SvcCall c(SvcOp::PickRead, "", u32(index));
    if (!c.ok())
        co_return Err(Error::NoMemory);
    c.set_ref(p.set.slot());
    c.req().h.result_lo = u32(off);
    c.req().h.result_hi = u32(off >> 32);

    Task<Result<String>> t = svc_chunk(c);
    if (!t)
        co_return Err(Error::NoMemory);
    co_return co_await t;
}

Task<Result<void>> save_file(Str name, Str bytes)
{
    SvcCall c(SvcOp::Save, name, 0);
    if (!c.ok() || !c.put(bytes))
        co_return Err(Error::NoMemory);
    CO_TRY_VOID(co_await c);
    co_return {};
}

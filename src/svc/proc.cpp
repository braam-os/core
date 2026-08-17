#include "proc.h"

#include "kernel/host.h"

Task<Result<void>> proc_spawn(u32 pid, Str path, String &&image, const ProcMeta &meta)
{
    // The page counts ride in `flags`: the host has to size the Memory it
    // supplies before it can instantiate. `aux` is the pid and nothing else
    // may ride on that.
    SvcCall c(SvcOp::ProcSpawn, path, proc_pack(meta));
    if (!c.ok())
        co_return Err(Error::NoMemory);
    c.set_aux(pid);
    if (!c.put(move(image)))
        co_return Err(Error::NoMemory);
    CO_TRY_VOID(co_await c);
    co_return {};
}

Task<Result<ProcStep>> proc_step(u32 pid, u32 token, Str payload, u32 *pages)
{
    SvcCall c(SvcOp::ProcStep, "", token);
    if (!c.ok())
        co_return Err(Error::NoMemory);
    c.set_aux(pid);
    if (!c.put(payload))
        co_return Err(Error::NoMemory);
    CO_TRY_VOID(co_await c);
    if (pages)
        *pages = c.req().h.result_hi;
    co_return ProcStep(c.req().h.result_lo);
}

void proc_kill(u32 pid)
{
    host_svc(u32(SvcOp::ProcKill), 0, pid, jsref_get(0));
}

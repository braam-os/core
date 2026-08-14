#include "proc.h"

#include "kernel/host.h"

Task<Result<void>> proc_spawn(u32 pid, Str path, String &&image, const ProcMeta &meta, Tier tier)
{
    // The page counts and the tier ride in `flags`: the host has to size the
    // Memory it supplies before it can instantiate, and has to know where to
    // put the instance. `aux` is the pid and nothing else may ride on that.
    SvcCall c(SvcOp::ProcSpawn, path, proc_pack(meta, tier));
    if (!c.ok())
        co_return Err(Error::NoMemory);
    c.set_aux(pid);
    if (!c.put(move(image)))
        co_return Err(Error::NoMemory);
    CO_TRY_VOID(co_await c);
    co_return {};
}

Task<Result<ProcStep>> proc_step(u32 pid, Str payload)
{
    SvcCall c(SvcOp::ProcStep, "", 0);
    if (!c.ok())
        co_return Err(Error::NoMemory);
    c.set_aux(pid);
    if (!c.put(payload))
        co_return Err(Error::NoMemory);
    CO_TRY_VOID(co_await c);
    co_return ProcStep(c.req().h.result_lo);
}

void proc_kill(u32 pid)
{
    host_svc(u32(SvcOp::ProcKill), 0, pid, jsref_get(0));
}

#include "svc.h"

#include "kernel/sysabi.h"

Task<Result<void>> svc_verify(Str key, Str sig, Str bytes)
{
    u8 head[8];
    sys_put_u32(head, u32(key.size()));
    sys_put_u32(head + 4, u32(sig.size()));

    String buf;
    if (!buf.append(Str(reinterpret_cast<const char *>(head), sizeof(head))) ||
        !buf.append(key) || !buf.append(sig) || !buf.append(bytes))
        co_return Err(Error::NoMemory);

    SvcCall c(SvcOp::Verify, "", 0);
    if (!c.ok() || !c.put(move(buf)))
        co_return Err(Error::NoMemory);
    CO_TRY_VOID(co_await c);
    co_return {};
}

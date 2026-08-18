#include "cmd/sh/job.h"
#include "decl.h"

// A builtin because the descriptors it keeps are this shell's own: a child
// would take them and exit.
Task<i32> builtin_exec(Args args, ShIo io)
{
    Task<i32> t = sh_exec(args, io);
    co_return t ? co_await t : 1;
}

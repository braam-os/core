#include "decl.h"
#include "sh/job.h"

// The arguments joined and run as a command, in this shell rather than a child.
Task<i32> builtin_eval(Args args, ShIo io)
{
    Task<i32> t = sh_eval(args, io);
    co_return t ? co_await t : 1;
}

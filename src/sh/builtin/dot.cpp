#include "decl.h"
#include "sh/job.h"

// A builtin because a sourced file sets this shell's variables and defines its
// functions: a child would fill its own and exit.
Task<i32> builtin_dot(Args args, ShIo io)
{
    if (args.size() != 2) {
        co_await write_all(io.err, "usage: . <file>\n");
        co_return 2;
    }

    Task<i32> t = sh_source(args[1], io);
    co_return t ? co_await t : 1;
}

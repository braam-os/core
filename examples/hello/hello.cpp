// The smallest Braam program that does something: it greets the world, or
// whoever it is given. doc/SDK.md walks through it.
#include "proc/io.h"

Task<i32> proc_main(Args args)
{
    Str who = "world";
    if (args.size() > 1)
        who = args[1];

    if ((co_await write_all(SYS_STDOUT, "Hello, ")).is_err() ||
        (co_await write_all(SYS_STDOUT, who)).is_err() ||
        (co_await write_all(SYS_STDOUT, "!\n")).is_err())
        co_return 1;

    co_return 0;
}

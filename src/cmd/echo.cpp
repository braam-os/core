#include "proc/io.h"

Task<i32> proc_main(Args args)
{
    usize i      = 1;
    bool newline = true;
    if (i < args.size() && args[i] == "-n") {
        newline = false;
        i++;
    }

    for (bool first = true; i < args.size(); i++, first = false) {
        if (!first && (co_await write_all(SYS_STDOUT, " ")).is_err())
            co_return 1;
        if ((co_await write_all(SYS_STDOUT, args[i])).is_err())
            co_return 1;
    }
    if (newline && (co_await write_all(SYS_STDOUT, "\n")).is_err())
        co_return 1;

    co_return 0;
}

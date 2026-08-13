#include "user/io.h"
#include "user/prog.h"

BRAAM_PROGRAM(prog_echo, "echo", "[-n] [word...] — write the arguments")
{
    usize i      = 1;
    bool newline = true;
    if (i < args.size() && args[i] == "-n") {
        newline = false;
        i++;
    }

    for (bool first = true; i < args.size(); i++, first = false) {
        if (!first && (co_await write_all(io.out, " ")).is_err())
            co_return 1;
        if ((co_await write_all(io.out, args[i])).is_err())
            co_return 1;
    }
    if (newline && (co_await write_all(io.out, "\n")).is_err())
        co_return 1;

    co_return 0;
}

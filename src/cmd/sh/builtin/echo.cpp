#include "decl.h"
#include "kernel/string.h"

// The one difference from /bin/echo: buffered and written once. A builtin runs
// in its turn, so a write per word would fill an eight-slot pipe and park with
// nobody left to drain it (builtin.h).
Task<i32> builtin_echo(Args args, ShIo io)
{
    usize i      = 1;
    bool newline = true;
    if (i < args.size() && args[i] == "-n") {
        newline = false;
        i++;
    }

    String out;
    for (bool first = true; i < args.size(); i++, first = false)
        if ((!first && !out.push(' ')) || !out.append(args[i]))
            co_return 1;
    if (newline && !out.push('\n'))
        co_return 1;

    if (Task<Result<void>> t = write_all(io.out, out.str()))
        co_return (co_await t).is_ok() ? 0 : 1;
    co_return 1;
}

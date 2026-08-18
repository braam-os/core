#include "decl.h"
#include "kernel/string.h"
#include "sh/var.h"

// `unset -f` is a later stage's, when there are functions to unset.
Task<i32> builtin_unset(Args args, ShIo io)
{
    if (args.size() < 2) {
        co_await write_all(io.err, "usage: unset <name>...\n");
        co_return 2;
    }

    // Every name is tried and the refusals reported in one write (builtin.h).
    String bad;
    for (usize i = 1; i < args.size(); i++) {
        if (var_unset(args[i]))
            continue;
        if (!bad.append("unset: ") || !bad.append(args[i]) || !bad.append(": is read only\n"))
            co_return 1;
    }

    if (bad.empty())
        co_return 0;
    if (Task<Result<void>> t = write_all(io.err, bad.str()))
        co_await t;
    co_return 1;
}

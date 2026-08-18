#include "decl.h"
#include "kernel/string.h"
#include "sh/job.h"
#include "sh/var.h"

Task<i32> builtin_unset(Args args, ShIo io)
{
    usize first = 1;
    bool funcs  = args.size() > 1 && args[1] == "-f";
    if (funcs)
        first = 2;

    if (args.size() <= first) {
        co_await write_all(io.err, "usage: unset [-f] <name>...\n");
        co_return 2;
    }

    // Every name is tried and the refusals reported in one write (builtin.h).
    String bad;
    for (usize i = first; i < args.size(); i++) {
        if (funcs) {
            // An absent one is success, as an absent variable is.
            func_unset(args[i]);
            continue;
        }
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

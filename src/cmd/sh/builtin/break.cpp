#include "cmd/sh/job.h"
#include "decl.h"
#include "kernel/text.h"

namespace {

// Builtins because what they touch is this process's own walk. They ask rather
// than act, as `exit` does, and the loop picks it up.
Task<i32> ask(Args args, ShIo io, bool cont)
{
    u32 n    = 1;
    bool bad = args.size() > 2;

    if (!bad && args.size() == 2) {
        // A literal per branch, not one conditional expression: Str's length
        // folds only when the pointer is constant (Concept.md §C.3).
        Option<u32> v = parse_u32(args[1]);
        if (!v || v.value() == 0)
            bad = true;
        else
            n = v.value();
    }
    if (bad) {
        if (cont)
            co_await write_all(io.err, "usage: continue [<n>]\n");
        else
            co_await write_all(io.err, "usage: break [<n>]\n");
        co_return 2;
    }

    loop_break(n, cont);
    co_return 0;
}

} // namespace

Task<i32> builtin_break(Args args, ShIo io)
{
    Task<i32> t = ask(args, io, false);
    co_return t ? co_await t : 1;
}

Task<i32> builtin_continue(Args args, ShIo io)
{
    Task<i32> t = ask(args, io, true);
    co_return t ? co_await t : 1;
}

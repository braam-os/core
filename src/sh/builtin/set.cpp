#include "decl.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "sh/var.h"

// Builtins because the table and the positional parameters are this process's
// own: a `set` in /bin would fill a child's and exit.

// With no operands, the variables; otherwise the positional parameters. The
// option letters `-e -x -u` are a later stage's.
Task<i32> builtin_set(Args args, ShIo io)
{
    if (args.size() > 1 && args[1].size() > 1 && args[1][0] == '-' && args[1] != "--") {
        co_await write_all(io.err, "usage: set [--] [<arg>...]\n");
        co_return 2;
    }

    if (args.size() == 1) {
        String out;
        for (usize i = 0; i < var_count(); i++) {
            const VarEntry *e = var_at(i);
            if (!out.append(e->name.str()) || !out.push('='))
                co_return 1;
            if (e->valued && !out.append(e->value.str()))
                co_return 1;
            if (!out.push('\n'))
                co_return 1;
        }
        if (Task<Result<void>> t = write_all(io.out, out.str()))
            co_return (co_await t).is_ok() ? 0 : 1;
        co_return 1;
    }

    Args rest = args.tail();
    if (rest.size() && rest[0] == "--")
        rest = rest.tail();
    co_return args_set(rest) ? 0 : 1;
}

Task<i32> builtin_shift(Args args, ShIo io)
{
    if (args.size() > 2) {
        co_await write_all(io.err, "usage: shift [<n>]\n");
        co_return 2;
    }

    u32 n = 1;
    if (args.size() == 2) {
        Option<u32> v = parse_u32(args[1]);
        if (!v) {
            co_await write_all(io.err, "usage: shift [<n>]\n");
            co_return 2;
        }
        n = v.value();
    }
    co_return args_shift(n) ? 0 : 1;
}

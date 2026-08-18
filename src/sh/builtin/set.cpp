#include "decl.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "sh/job.h"
#include "sh/var.h"

// Builtins because the table, the option letters and the positional parameters
// are this process's own: a `set` in /bin would fill a child's and exit.

// The options first, then the parameters — which `set -x` must leave alone and
// `set --` must clear. With nothing at all, the variables.
Task<i32> builtin_set(Args args, ShIo io)
{
    usize at   = 1;
    u32 flags  = sh_flags();
    bool ended = false;
    for (; at < args.size(); at++) {
        Str w = args[at];
        if (w == "--") {
            ended = true;
            at++;
            break;
        }
        if (w.size() < 2 || (w[0] != '-' && w[0] != '+'))
            break;

        bool on = w[0] == '-';
        for (usize k = 1; k < w.size(); k++) {
            u32 bit = sh_flag_of(w[k]);
            if (!bit) {
                co_await write_all(io.err, "usage: set [-eux] [+eux] [--] [<arg>...]\n");
                co_return 2;
            }
            flags = on ? (flags | bit) : (flags & ~bit);
        }
    }
    sh_set_flags(flags);

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

    // Only when operands were given: `set -x` is not `set --`.
    if (ended || at < args.size())
        co_return args_set(Args{ args.v.subspan(at) }) ? 0 : 1;
    co_return 0;
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

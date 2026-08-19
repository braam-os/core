#include "cmd/sh/name.h"
#include "cmd/sh/var.h"
#include "decl.h"
#include "kernel/string.h"

namespace {

// The two differ only in the flag. Each operand is `name` or `name=value`.
Task<i32> mark(Args args, ShIo io, bool exported)
{
    Str who = exported ? Str("export") : Str("readonly");

    if (args.size() == 1) {
        String out;
        for (usize i = 0; i < var_count(); i++) {
            const VarEntry *e = var_at(i);
            if (exported ? !e->exported : !e->readonly)
                continue;
            if (!out.append(who) || !out.push(' ') || !out.append(e->name.str()))
                co_return 1;
            if (e->valued && (!out.push('=') || !out.append(e->value.str())))
                co_return 1;
            if (!out.push('\n'))
                co_return 1;
        }
        if (Task<Result<void>> t = write_all(io.out, out.str()))
            co_return (co_await t).is_ok() ? 0 : 1;
        co_return 1;
    }

    String bad;
    for (usize i = 1; i < args.size(); i++) {
        Str w    = args[i];
        usize eq = w.find('=');
        Str name = eq == Str::npos ? w : w.substr(0, eq);

        // Before the value: an illegal name sets nothing and marks nothing.
        if (!is_name(name)) {
            if (!bad.append(who) || !bad.append(": ") || !bad.append(name) ||
                !bad.append(": not a valid name\n"))
                co_return 1;
            continue;
        }

        // The value first: the mark this call makes would refuse it after.
        bool ok = eq == Str::npos || var_set(name, w.substr(eq + 1));
        if (ok)
            ok = var_mark(name, exported, !exported);
        if (ok)
            continue;
        if (!bad.append(who) || !bad.append(": ") || !bad.append(name) ||
            !bad.append(": cannot be set\n"))
            co_return 1;
    }

    if (bad.empty())
        co_return 0;
    if (Task<Result<void>> t = write_all(io.err, bad.str()))
        co_await t;
    co_return 1;
}

} // namespace

// A marked variable goes into every child's environment from here on: the
// stage builds one out of the table at each spawn (job.cpp).
Task<i32> builtin_export(Args args, ShIo io)
{
    Task<i32> t = mark(args, io, true);
    co_return t ? co_await t : 1;
}

// This one bites: `var_set` refuses a name it has marked.
Task<i32> builtin_readonly(Args args, ShIo io)
{
    Task<i32> t = mark(args, io, false);
    co_return t ? co_await t : 1;
}

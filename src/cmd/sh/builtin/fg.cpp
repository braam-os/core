// Brings a background job back to the foreground: the console is handed to it,
// so what is typed reaches its stdin and ^C cancels it rather than us, and we
// wait for it to stop.
//
// Both halves are jobs_wait's, because both are syscalls the shell makes on its
// own behalf — Sys::Fg for each of the job's stages, then giving the keyboard
// back so that a job which wants raw keys can claim them.
#include "cmd/sh/job.h"
#include "decl.h"
#include "kernel/string.h"
#include "kernel/text.h"

Task<i32> builtin_fg(Args args, ShIo io)
{
    if (args.size() > 2) {
        co_await write_all(io.err, "usage: fg [%n]\n");
        co_return 2;
    }

    u32 id = jobs_current();
    if (args.size() == 2) {
        Str a = args[1];
        if (a.starts_with("%"))
            a = a.substr(1);
        Option<u32> n = parse_u32(a);
        if (!n) {
            if (Task<void> e = errln("fg", args[1], Error::Invalid))
                co_await e;
            co_return 2;
        }
        id = n.value();
    }

    JobInfo j;
    if (!id || !jobs_find(id, j)) {
        co_await write_all(io.err, "fg: no such job\n");
        co_return 1;
    }

    String echo;
    if (!echo.assign(j.cmd) || !echo.push('\n'))
        co_return 1;
    if ((co_await write_all(io.out, echo.str())).is_err())
        co_return 1;

    Task<Result<i32>> t = jobs_wait(id, true);
    if (!t)
        co_return 1;
    Result<i32> r = co_await t;
    if (r.is_err())
        co_return r.error() == Error::Cancelled ? 130 : 1;
    co_return r.value();
}

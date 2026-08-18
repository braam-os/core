#include "cmd/sh/job.h"
#include "decl.h"
#include "kernel/text.h"
#include "kernel/vec.h"

// A builtin because the job table is the shell's own memory, as `jobs` and
// `kill` are. It puts the job in front while it waits, which v7's `wait` does
// not: there are no signals, so being in front is the only way a ^C can reach
// anything (../../user/console.h). `fg` is then this plus the echo.
Task<i32> builtin_wait(Args args, ShIo io)
{
    // The ids first: jobs_wait drops the entry it collected, so the table
    // shifts under a walk of it.
    Vec<u32> ids;
    if (args.size() == 1) {
        JobInfo j;
        for (usize i = 0; jobs_at(i, j); i++)
            if (!ids.push(j.id))
                co_return 1;
    } else {
        for (usize i = 1; i < args.size(); i++) {
            Str a = args[i];
            if (a.starts_with("%"))
                a = a.substr(1);
            Option<u32> n = parse_u32(a);
            if (!n || !n.value()) {
                if (Task<void> e = errln("wait", args[i], Error::Invalid))
                    co_await e;
                co_return 2;
            }
            if (!ids.push(n.value()))
                co_return 1;
        }
    }

    i32 status = 0;
    for (u32 id : ids) {
        Task<Result<i32>> t = jobs_wait(id, sh_interactive());
        Result<i32> r       = t ? co_await t : Err(Error::NoMemory);
        if (r.is_ok()) {
            status = r.value();
            continue;
        }
        if (r.error() == Error::Cancelled)
            co_return 130;
        co_await write_all(io.err, "wait: no such job\n");
        status = 127;
    }
    co_return status;
}

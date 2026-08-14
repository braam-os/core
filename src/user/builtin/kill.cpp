// Cancellation, which is all a kill can be for anything cooperative: a
// coroutine stops at its next await point and unwinds by returning. A program
// that never awaits cannot be stopped that way at all — that is what M9's
// own-worker tier and worker.terminate() are for, and a tier-3 process really
// does die here. A builtin because the job table is the shell's.
#include "decl.h"
#include "kernel/sched.h"
#include "kernel/text.h"
#include "user/io.h"
#include "user/job.h"

Task<i32> builtin_kill(Args args, Stdio io)
{
    if (args.size() < 2) {
        co_await io.err.write("usage: kill %n | pid...\n");
        co_return 2;
    }

    i32 status = 0;
    for (usize i = 1; i < args.size(); i++) {
        Str a    = args[i];
        bool job = a.starts_with("%");
        if (job)
            a = a.substr(1);

        Option<u32> n = parse_u32(a);
        if (!n || !n.value()) {
            co_await io.err.write("kill: not a job or a pid: ");
            co_await io.err.write(args[i]);
            co_await io.err.write("\n");
            status = 1;
            continue;
        }

        bool ok = job ? jobs_kill(n.value()) : sched_alive(n.value());
        if (!job && ok)
            sched_cancel(n.value());
        if (!ok) {
            co_await io.err.write("kill: no such ");
            co_await io.err.write(job ? "job\n" : "process\n");
            status = 1;
        }
    }
    co_return status;
}

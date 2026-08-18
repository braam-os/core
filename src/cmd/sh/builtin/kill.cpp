// Cancellation, which is all a kill can be for anything cooperative: a
// coroutine stops at its next await point and unwinds by returning. A program
// that never awaits cannot be stopped that way at all — that is what M9's
// worker and worker.terminate() are for, and a process really does die here.
//
// Jobs only. `Sys::Kill` refuses anything that is not a child of the caller
// (Concept.md §4.3), and a bare pid the shell did not start is exactly that —
// so `kill 12` is gone, and the authority it needed was never the shell's to
// have once the shell became a process.
#include "cmd/sh/job.h"
#include "decl.h"
#include "kernel/string.h"
#include "kernel/text.h"

Task<i32> builtin_kill(Args args, ShIo io)
{
    if (args.size() < 2) {
        co_await write_all(io.err, "usage: kill %n...\n");
        co_return 2;
    }

    i32 status = 0;
    String bad;
    for (usize i = 1; i < args.size(); i++) {
        Str a = args[i];
        if (a.starts_with("%"))
            a = a.substr(1);

        Option<u32> n = parse_u32(a);
        bool ok       = false;
        if (n && n.value())
            if (Task<Result<void>> t = jobs_kill(n.value()))
                ok = (co_await t).is_ok();
        if (!ok) {
            bad.append("kill: no such job\n");
            status = 1;
        }
    }

    if (!bad.empty())
        co_await write_all(io.err, bad.str());
    co_return status;
}

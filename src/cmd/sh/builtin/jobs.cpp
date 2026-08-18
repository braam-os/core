#include "cmd/sh/job.h"
#include "decl.h"
#include "kernel/fmt.h"
#include "kernel/string.h"

// A builtin because the job table is the shell's own memory, and no syscall
// shows one process another's.
//
// One write at the end rather than one per line: a builtin in a pipeline runs
// to completion in its turn, so a line at a time would fill the pipe and park
// with nobody left to drain it (builtin.h).
Task<i32> builtin_jobs(Args args, ShIo io)
{
    if (args.size() != 1) {
        co_await write_all(io.err, "usage: jobs\n");
        co_return 2;
    }

    String out;
    JobInfo j;
    for (usize i = 0; jobs_at(i, j); i++) {
        Buf<96> line;
        line.put('[').put(j.id).put(jobs_current() == j.id ? "]+ " : "]  ");
        line.put(j.running ? "running " : "done    ");
        line.put(j.cmd).put('\n');
        if (!out.append(line.str()))
            co_return 1;
    }

    if (Task<Result<void>> t = write_all(io.out, out.str()))
        co_return (co_await t).is_ok() ? 0 : 1;
    co_return 1;
}

#include "kernel/fmt.h"
#include "user/io.h"
#include "user/job.h"
#include "user/prog.h"

BRAAM_PROGRAM(prog_jobs, "jobs", "list the jobs started with &")
{
    if (args.size() != 1) {
        co_await io.err.write("usage: jobs\n");
        co_return 2;
    }

    JobInfo j;
    for (usize i = 0; jobs_at(i, j); i++) {
        Buf<96> line;
        line.put('[').put(j.id).put(jobs_current() == j.id ? "]+ " : "]  ");
        line.put(j.running ? "running " : "done    ");
        line.put(j.cmd).put('\n');
        if ((co_await write_all(io.out, line.str())).is_err())
            co_return 1;
    }
    co_return 0;
}

// Brings a background job back to the foreground: what is typed goes to its
// stdin instead of ours, and we wait for it.
//
// `fg` never touches the keyboard itself. The pump that is running belongs to
// the pipeline `fg` is a stage of, and there is only ever one receiver on
// keys(); an InputClaim tells that pump where to put the bytes instead. ^C
// therefore cancels *us*, and the destructor below passes it on to the job we
// adopted — which is also what makes the wait cancellable at all.
#include "decl.h"
#include "kernel/fmt.h"
#include "kernel/text.h"
#include "user/io.h"
#include "user/job.h"
#include "user/tty.h"

Task<i32> builtin_fg(Args args, Stdio io)
{
    if (args.size() > 2) {
        co_await io.err.write("usage: fg [%n]\n");
        co_return 2;
    }

    u32 id = jobs_current();
    if (args.size() == 2) {
        Str a = args[1];
        if (a.starts_with("%"))
            a = a.substr(1);
        Option<u32> n = parse_u32(a);
        if (!n) {
            co_await io.err.write("fg: not a job: ");
            co_await io.err.write(args[1]);
            co_await io.err.write("\n");
            co_return 2;
        }
        id = n.value();
    }

    JobInfo j;
    if (!id || !jobs_find(id, j)) {
        co_await io.err.write("fg: no such job\n");
        co_return 1;
    }

    if ((co_await write_all(io.out, j.cmd)).is_err())
        co_return 1;
    if ((co_await write_all(io.out, "\n")).is_err())
        co_return 1;

    struct Adopted {
        ~Adopted()
        {
            if (killed)
                jobs_kill(id);
        }

        u32 id;
        bool killed = true;
    } adopted{ id };

    Task<Result<i32>> t = jobs_wait(id);
    if (!t)
        co_return 1;

    InputClaim claim(jobs_input(id));

    Result<i32> r = co_await t;

    // Only a normal finish disarms the destructor: an observed cancellation
    // gets here too, and the job we adopted must go with us.
    if (r.is_err())
        co_return r.error() == Error::Cancelled ? 130 : 1;
    adopted.killed = false;
    co_return r.value();
}

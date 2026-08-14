#include "proc/io.h"

// A syscall rather than /proc/cwd, which is where this read until every process
// got a working directory of its own (Concept.md §5.1). /proc is generated at
// open and cannot know which process is reading, so the file there is the
// shell's cwd and only the kernel can say what *this* process's is.
Task<i32> proc_main(Args args)
{
    if (args.size() > 1) {
        co_await write_all(SYS_STDERR, "usage: pwd\n");
        co_return 2;
    }

    Result<String> r = Err(Error::NoMemory);
    if (Task<Result<String>> t = cwd_get())
        r = co_await t;
    if (r.is_err()) {
        if (r.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("pwd", "cwd", r.error()))
            co_await e;
        co_return 1;
    }

    if (!r.value().push('\n'))
        co_return 1;
    if ((co_await write_all(SYS_STDOUT, r.value().str())).is_err())
        co_return 1;
    co_return 0;
}

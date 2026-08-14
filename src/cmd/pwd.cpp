#include "proc/io.h"

// The working directory is one global the kernel publishes as text, so this
// needs no operation of its own: /proc is the interface, and `cat /proc/cwd`
// says the same thing (Concept.md §5.1).
Task<i32> proc_main(Args args)
{
    if (args.size() > 1) {
        co_await write_all(SYS_STDERR, "usage: pwd\n");
        co_return 2;
    }

    Result<String> r = Err(Error::NoMemory);
    if (Task<Result<String>> t = read_file("/proc/cwd"))
        r = co_await t;
    if (r.is_err()) {
        if (r.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("pwd", "/proc/cwd", r.error()))
            co_await e;
        co_return 1;
    }

    if ((co_await write_all(SYS_STDOUT, r.value().str())).is_err())
        co_return 1;
    co_return 0;
}

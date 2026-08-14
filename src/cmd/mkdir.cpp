#include "proc/io.h"

Task<i32> proc_main(Args args)
{
    if (args.size() < 2) {
        co_await write_all(SYS_STDERR, "usage: mkdir <dir>...\n");
        co_return 2;
    }

    i32 status = 0;
    for (usize i = 1; i < args.size(); i++) {
        Result<void> r = Err(Error::NoMemory);
        if (Task<Result<void>> t = make_dir(args[i]))
            r = co_await t;
        if (r.is_ok())
            continue;
        if (r.error() == Error::Cancelled)
            co_return 130;

        status = 1;
        if (Task<void> e = errln("mkdir", args[i], r.error()))
            co_await e;
    }
    co_return status;
}

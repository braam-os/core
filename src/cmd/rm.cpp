#include "proc/io.h"

Task<i32> proc_main(Args args)
{
    bool all = false;
    usize i  = 1;
    for (; i < args.size(); i++) {
        if (args[i] == "-r")
            all = true;
        else
            break;
    }
    if (i >= args.size()) {
        co_await write_all(SYS_STDERR, "usage: rm [-r] <path>...\n");
        co_return 2;
    }

    i32 status = 0;
    for (; i < args.size(); i++) {
        Result<void> r = Err(Error::NoMemory);
        if (Task<Result<void>> t = remove_path(args[i], all))
            r = co_await t;
        if (r.is_ok())
            continue;
        if (r.error() == Error::Cancelled)
            co_return 130;

        status = 1;
        if (Task<void> e = errln("rm", args[i], r.error()))
            co_await e;
    }
    co_return status;
}

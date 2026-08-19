#include "proc/io.h"

// Moves an existing file's mtime to now, and creates one that is not there. It
// exists because `> file` is the only other way to make an empty file, and that
// reads like a mistake.
Task<i32> proc_main(Args args)
{
    if (args.size() < 2) {
        co_await write_all(SYS_STDERR, "usage: touch <file>...\n");
        co_return 2;
    }

    i32 status = 0;
    for (usize i = 1; i < args.size(); i++) {
        Result<void> r = Err(Error::NoMemory);
        if (Task<Result<void>> t = touch_path(args[i]))
            r = co_await t;
        if (r.is_ok())
            continue;
        if (r.error() == Error::Cancelled)
            co_return 130;

        if (r.error() == Error::NotFound) {
            Result<i32> o = Err(Error::NoMemory);
            if (Task<Result<i32>> t = open_at(args[i], SYS_O_WRITE | SYS_O_CREATE))
                o = co_await t;
            if (o.is_ok()) {
                if (Task<void> c = close_fd(u32(o.value())))
                    co_await c;
                continue;
            }
            if (o.error() == Error::Cancelled)
                co_return 130;
            r = Err(o.error());
        }

        status = 1;
        if (Task<void> e = errln("touch", args[i], r.error()))
            co_await e;
    }
    co_return status;
}

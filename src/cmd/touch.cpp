#include "proc/io.h"

// There are no timestamps yet, so this creates and nothing else. It exists
// because `> file` is the only other way to make an empty file, and that reads
// like a mistake.
Task<i32> proc_main(Args args)
{
    if (args.size() < 2) {
        co_await write_all(SYS_STDERR, "usage: touch <file>...\n");
        co_return 2;
    }

    i32 status = 0;
    for (usize i = 1; i < args.size(); i++) {
        Result<i32> r = Err(Error::NoMemory);
        if (Task<Result<i32>> t = open_at(args[i], SYS_O_WRITE | SYS_O_CREATE))
            r = co_await t;
        if (r.is_ok()) {
            if (Task<void> c = close_fd(u32(r.value())))
                co_await c;
            continue;
        }
        if (r.error() == Error::Cancelled)
            co_return 130;

        status = 1;
        if (Task<void> e = errln("touch", args[i], r.error()))
            co_await e;
    }
    co_return status;
}

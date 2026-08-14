#include "kernel/text.h"
#include "proc/io.h"

// Milliseconds, not seconds: there is no float parser, the scheduler is a
// millisecond machine, and the smoke test needs an exact number to assert
// tick() against.
Task<i32> proc_main(Args args)
{
    Option<u32> ms = args.size() == 2 ? parse_u32(args[1]) : None;
    if (!ms.has_value()) {
        co_await write_all(SYS_STDERR, "usage: sleep <ms>\n");
        co_return 2;
    }

    Result<void> r = Err(Error::NoMemory);
    if (Task<Result<void>> t = sleep_for(ms.value()))
        r = co_await t;
    if (r.is_err())
        co_return 130;

    co_return 0;
}

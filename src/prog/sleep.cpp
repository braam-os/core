#include "kernel/sched.h"
#include "kernel/text.h"
#include "user/prog.h"

// Milliseconds, not seconds: there is no float parser, the scheduler is a
// millisecond machine, and the smoke test needs an exact number to assert
// tick() against.
BRAAM_PROGRAM(prog_sleep, "sleep", "<ms> — wait, in milliseconds") {
    Option<u32> ms = args.size() == 2 ? parse_u32(args[1]) : None;
    if (!ms.has_value()) {
        co_await io.err.write("usage: sleep <ms>\n");
        co_return 2;
    }

    if ((co_await sleep_ms(ms.value())).is_err())
        co_return 130;

    co_return 0;
}

#include "kernel/text.h"
#include "proc/io.h"

// Seconds, or milliseconds with -m.

namespace {

constexpr u32 MAX_SECS = 4294967; // as many as convert to ms inside a u32

} // namespace

Task<i32> proc_main(Args args)
{
    usize i    = 1;
    bool milli = i < args.size() && args[i] == "-m";
    if (milli)
        i++;

    Option<u32> n = args.size() == i + 1 ? parse_u32(args[i]) : None;
    if (!n.has_value() || (!milli && n.value() > MAX_SECS)) {
        co_await write_all(SYS_STDERR, "usage: sleep [-m] <seconds>\n");
        co_return 2;
    }
    u32 ms = milli ? n.value() : n.value() * 1000;

    Result<void> r = Err(Error::NoMemory);
    if (Task<Result<void>> t = sleep_for(ms))
        r = co_await t;
    if (r.is_err())
        co_return 130;

    co_return 0;
}

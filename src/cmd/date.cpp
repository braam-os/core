#include "kernel/fmt.h"
#include "proc/io.h"
#include "proc/time.h"

namespace {

void put2(Buf<64> &b, u32 v)
{
    b.put(char('0' + (v / 10) % 10)).put(char('0' + v % 10));
}

} // namespace

// The kernel's clock is monotonic — host_now() is performance.now(), which is
// what the timer queue wants and cannot name a day. The wall clock is one
// syscall, so `date` asks each time rather than caching an origin.
Task<i32> proc_main(Args args)
{
    bool utc = args.size() > 1 && args[1] == "-u";
    if (args.size() > 2 || (args.size() == 2 && !utc)) {
        co_await write_all(SYS_STDERR, "usage: date [-u]\n");
        co_return 2;
    }

    Result<Clock> got = Err(Error::NoMemory);
    if (Task<Result<Clock>> t = clock_now())
        got = co_await t;
    if (got.is_err()) {
        if (got.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("date", Str(), got.error()))
            co_await e;
        co_return 1;
    }

    i32 tz   = utc ? 0 : got.value().tz_min;
    i64 secs = i64(got.value().epoch_ms / 1000) + tz * 60;
    Civil c  = civil(secs);

    Buf<64> line;
    line.put(TIME_DAYS[c.weekday]).put(' ').put(TIME_MONTHS[c.month - 1]).put(' ');
    put2(line, c.day);
    line.put(' ');
    put2(line, c.hour);
    line.put(':');
    put2(line, c.min);
    line.put(':');
    put2(line, c.sec);
    line.put(' ').put(tz < 0 ? '-' : '+');
    u32 off = u32(tz < 0 ? -tz : tz);
    put2(line, off / 60);
    put2(line, off % 60);
    line.put(' ').put(u32(c.year)).put('\n');

    if ((co_await write_all(SYS_STDOUT, line.str())).is_err())
        co_return 1;
    co_return 0;
}

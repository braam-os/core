#include "kernel/fmt.h"
#include "proc/io.h"

namespace {

constexpr Str MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
constexpr Str DAYS[]   = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };

struct Civil {
    i32 year;
    u32 month, day, hour, min, sec, weekday;
};

// Days since 1970-01-01 to a calendar date, by way of an era of 400 years —
// the usual branch-free civil_from_days, which needs no leap-year table.
Civil civil(i64 secs)
{
    i64 days = secs / 86400;
    i64 rem  = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }

    Civil c;
    c.weekday = u32(((days % 7) + 7) % 7);
    c.hour    = u32(rem / 3600);
    c.min     = u32((rem / 60) % 60);
    c.sec     = u32(rem % 60);

    i64 z   = days + 719468;
    i64 era = (z >= 0 ? z : z - 146096) / 146097;
    u64 doe = u64(z - era * 146097);
    u64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    u64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    u64 mp  = (5 * doy + 2) / 153;
    c.day   = u32(doy - (153 * mp + 2) / 5 + 1);
    c.month = u32(mp < 10 ? mp + 3 : mp - 9);
    c.year  = i32(i64(yoe) + era * 400) + (c.month <= 2 ? 1 : 0);
    return c;
}

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
    line.put(DAYS[c.weekday]).put(' ').put(MONTHS[c.month - 1]).put(' ');
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

#include "harness.h"
#include "proc/time.h"

namespace {

void check(i64 secs, i32 year, u32 month, u32 day, u32 hour, u32 min, u32 sec, Str weekday)
{
    Civil c = civil(secs);
    CHECK_EQ(c.year, year);
    CHECK_EQ(c.month, month);
    CHECK_EQ(c.day, day);
    CHECK_EQ(c.hour, hour);
    CHECK_EQ(c.min, min);
    CHECK_EQ(c.sec, sec);
    CHECK(TIME_DAYS[c.weekday] == weekday);
}

} // namespace

void test_time()
{
    test_begin("time");

    check(0, 1970, 1, 1, 0, 0, 0, "Thu");
    check(86399, 1970, 1, 1, 23, 59, 59, "Thu");
    check(86400, 1970, 1, 2, 0, 0, 0, "Fri");

    // A leap day, and the day after it.
    check(1709208000, 2024, 2, 29, 12, 0, 0, "Thu");
    check(1709294400, 2024, 3, 1, 12, 0, 0, "Fri");

    // 1900 is not a leap year and 2000 is — the two the era arithmetic exists
    // to get right without a table.
    check(-2203891200, 1900, 3, 1, 0, 0, 0, "Thu");
    check(951782400, 2000, 2, 29, 0, 0, 0, "Tue");

    // Before the epoch, where the seconds-in-day remainder goes negative.
    check(-1, 1969, 12, 31, 23, 59, 59, "Wed");
    check(-86400, 1969, 12, 31, 0, 0, 0, "Wed");

    CHECK(TIME_MONTHS[0] == "Jan");
    CHECK(TIME_MONTHS[11] == "Dec");
}

// Calendar conversion, shared by `date` and by `ls -l`. Pure: no syscall, no
// host import, nothing that could not be compiled into the unit tests.
#pragma once

#include "kernel/str.h"
#include "kernel/types.h"

struct Civil {
    i32 year;
    u32 month, day, hour, min, sec, weekday; // month and day are 1-based
};

// Seconds since 1970-01-01 to a calendar date. Negative seconds work.
Civil civil(i64 secs);

extern const Str TIME_MONTHS[12]; // "Jan" ...
extern const Str TIME_DAYS[7];    // indexed by Civil::weekday, "Thu" first

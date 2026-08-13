#include "harness.h"

#include "kernel/fmt.h"
#include "kernel/host.h"

namespace {
Str current;
u32 failures;
} // namespace

void test_begin(Str name)
{
    current = name;
}

u32 test_failures()
{
    return failures;
}

void test_check(bool ok, Str expr, Str file, u32 line)
{
    if (ok)
        return;
    failures++;
    Buf<256> b;
    b.put("FAIL ").put(current).put(": ").put(expr).put(" at ").put(file).put(':').put(line);
    log(b.str());
}

void test_check_eq(u32 a, u32 b, Str expr, Str file, u32 line)
{
    if (a == b)
        return;
    failures++;
    Buf<256> m;
    m.put("FAIL ")
        .put(current)
        .put(": ")
        .put(expr)
        .put(" (")
        .put(a)
        .put(" vs ")
        .put(b)
        .put(") at ")
        .put(file)
        .put(':')
        .put(line);
    log(m.str());
}

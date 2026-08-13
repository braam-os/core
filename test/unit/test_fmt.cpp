#include "harness.h"

#include "kernel/fmt.h"

void test_fmt() {
    test_begin("fmt");

    Buf<64> b;
    b.put("n=").put(u32(0)).put(' ').put(u32(1234567890)).put(' ').put(-42);
    CHECK(b.str() == "n=0 1234567890 -42");
    CHECK(!b.overflowed());

    b.clear();
    CHECK(b.str().empty());
    b.put_hex(0xDEADBEEF);
    CHECK(b.str() == "0xdeadbeef");

    b.clear();
    b.put(-2147483647 - 1); // INT_MIN must not negate into overflow
    CHECK(b.str() == "-2147483648");

    // Overflow truncates and is reported rather than corrupting memory.
    Buf<4> small;
    small.put("abcdef");
    CHECK(small.overflowed());
    CHECK(small.str() == "abcd");
}

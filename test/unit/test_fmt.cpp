#include "harness.h"
#include "kernel/fmt.h"

void test_fmt()
{
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

    // Columns: padded to the width, and wider than it comes through whole.
    b.clear();
    b.put_right(u64(487), 9).put_right("55%", 7);
    CHECK(b.str() == "      487    55%");

    b.clear();
    b.put_left("opfs", 10).put_right(u64(10485760), 11);
    CHECK(b.str() == "opfs         10485760");

    b.clear();
    b.put_left("filesystem", 4).put_right(u64(123456), 3).put_right("-", 0);
    CHECK(b.str() == "filesystem123456-");

    // Overflow truncates and is reported rather than corrupting memory.
    Buf<4> small;
    small.put("abcdef");
    CHECK(small.overflowed());
    CHECK(small.str() == "abcd");
}

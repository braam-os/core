#include "harness.h"

#include "kernel/screen.h"
#include "kernel/text.h"

namespace {

// Encodes, then reads the codepoint back out of the grid through
// screen_write's decoder — the two halves have to agree.
void round_trip(char32_t ch, usize want_len) {
    char b[4];
    usize n = utf8_encode(ch, b);
    CHECK_EQ(n, want_len);

    screen_move(0, 0);
    screen_write(Str(b, n));
    CHECK_EQ(u32(screen_cells()[0].ch), u32(ch));
}

} // namespace

void test_text() {
    test_begin("text");

    CHECK(is_space(' '));
    CHECK(is_space('\t'));
    CHECK(is_space('\n'));
    CHECK(!is_space('a'));
    CHECK(!is_space('\0'));
    CHECK(is_digit('0'));
    CHECK(is_digit('9'));
    CHECK(!is_digit('/'));
    CHECK(!is_digit(':'));

    screen_reset();
    CHECK(screen_resize(8, 2));

    round_trip('A', 1);     // 1 byte
    round_trip(0x00e9, 2);  // e acute
    round_trip(0x20ac, 3);  // euro sign
    round_trip(0x1f600, 4); // an emoji

    // A surrogate and an out-of-range value both become U+FFFD.
    {
        char b[4];
        CHECK_EQ(utf8_encode(char32_t(0xd800), b), 3);
        CHECK_EQ(u8(b[0]), 0xef);
        CHECK_EQ(utf8_encode(char32_t(0x110000), b), 3);
        CHECK_EQ(u8(b[0]), 0xef);
    }

    screen_reset();

    CHECK(!parse_u32("").has_value());
    CHECK_EQ(parse_u32("0").value(), 0);
    CHECK_EQ(parse_u32("7").value(), 7);
    CHECK_EQ(parse_u32("4294967295").value(), 4294967295u);
    CHECK(!parse_u32("4294967296").has_value());
    CHECK(!parse_u32("99999999999").has_value());
    CHECK(!parse_u32("12a").has_value());
    CHECK(!parse_u32("+1").has_value());
    CHECK(!parse_u32(" 1").has_value());
    CHECK(!parse_u32("-1").has_value());
    CHECK_EQ(parse_u32("0000000030").value(), 30);
}

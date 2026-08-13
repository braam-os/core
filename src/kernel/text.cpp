#include "text.h"

usize utf8_encode(char32_t ch, char *out) {
    u32 v = u32(ch);
    if (v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
        v = 0xfffd;

    if (v < 0x80) {
        out[0] = char(v);
        return 1;
    }
    if (v < 0x800) {
        out[0] = char(0xc0 | (v >> 6));
        out[1] = char(0x80 | (v & 0x3f));
        return 2;
    }
    if (v < 0x10000) {
        out[0] = char(0xe0 | (v >> 12));
        out[1] = char(0x80 | ((v >> 6) & 0x3f));
        out[2] = char(0x80 | (v & 0x3f));
        return 3;
    }
    out[0] = char(0xf0 | (v >> 18));
    out[1] = char(0x80 | ((v >> 12) & 0x3f));
    out[2] = char(0x80 | ((v >> 6) & 0x3f));
    out[3] = char(0x80 | (v & 0x3f));
    return 4;
}

usize utf8_decode(Str s, usize at, char32_t &out) {
    if (at >= s.size())
        return 0;

    u8 c = u8(s[at]);
    char32_t ch;
    usize len;
    if (c < 0x80) {
        ch = c;
        len = 1;
    } else if ((c & 0xe0) == 0xc0) {
        ch = c & 0x1f;
        len = 2;
    } else if ((c & 0xf0) == 0xe0) {
        ch = c & 0x0f;
        len = 3;
    } else if ((c & 0xf8) == 0xf0) {
        ch = c & 0x07;
        len = 4;
    } else {
        out = 0xfffd;
        return 1;
    }

    if (at + len > s.size())
        return 0;
    for (usize k = 1; k < len; k++)
        ch = (ch << 6) | (u8(s[at + k]) & 0x3f);

    out = ch;
    return len;
}

Option<u32> parse_u32(Str s) {
    if (s.empty())
        return None;

    u32 v = 0;
    for (usize i = 0; i < s.size(); i++) {
        if (!is_digit(s[i]))
            return None;
        u32 d = u32(s[i] - '0');
        if (v > (0xffffffffu - d) / 10)
            return None;
        v = v * 10 + d;
    }
    return Option<u32>(v);
}

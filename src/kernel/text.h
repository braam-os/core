// Character classes, UTF-8 encoding and integer parsing. screen_write decodes
// UTF-8; this is the other direction, plus the scraps a tokeniser needs.
#pragma once

#include "result.h"
#include "str.h"
#include "types.h"

constexpr bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

constexpr bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// Encodes one codepoint into `out`, which must hold four bytes, and returns
// the length. A surrogate or an out-of-range value becomes U+FFFD.
usize utf8_encode(char32_t ch, char *out);

// Decodes the codepoint at s[at]. Returns the bytes consumed, or 0 when the
// sequence runs past the end. A stray continuation byte consumes one byte and
// yields U+FFFD, so bad input is visible rather than silently dropped.
usize utf8_decode(Str s, usize at, char32_t &out);

// Decimal, no sign, no leading space, and the whole string must be digits.
// None on empty input, a stray character, or a value past 2^32 - 1.
Option<u32> parse_u32(Str s);

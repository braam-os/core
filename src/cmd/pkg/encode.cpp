#include "encode.h"

namespace {

constexpr Str HEX    = "0123456789abcdef";
constexpr Str BASE64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// The digit's value, or 16.
constexpr u32 hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return u32(c - '0');
    if (c >= 'a' && c <= 'f')
        return u32(c - 'a') + 10;
    if (c >= 'A' && c <= 'F')
        return u32(c - 'A') + 10;
    return 16;
}

// The character's value, or 64.
constexpr u32 base64_value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return u32(c - 'A');
    if (c >= 'a' && c <= 'z')
        return u32(c - 'a') + 26;
    if (c >= '0' && c <= '9')
        return u32(c - '0') + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return 64;
}

} // namespace

bool hex_encode(Bytes in, Span<char> out)
{
    if (out.size() < hex_size(in.size()))
        return false;
    for (usize i = 0; i < in.size(); i++) {
        out[i * 2]     = HEX[in[i] >> 4];
        out[i * 2 + 1] = HEX[in[i] & 15];
    }
    return true;
}

Option<usize> hex_decode(Str in, Span<u8> out)
{
    if (in.size() % 2 != 0 || out.size() < in.size() / 2)
        return None;
    for (usize i = 0; i < in.size(); i += 2) {
        u32 hi = hex_value(in[i]), lo = hex_value(in[i + 1]);
        if (hi == 16 || lo == 16)
            return None;
        out[i / 2] = u8(hi << 4 | lo);
    }
    return in.size() / 2;
}

bool base64_encode(Bytes in, Span<char> out)
{
    if (out.size() < base64_size(in.size()))
        return false;
    usize at = 0;
    for (usize i = 0; i < in.size(); i += 3) {
        usize have = in.size() - i;
        u32 group  = u32(in[i]) << 16;
        if (have > 1)
            group |= u32(in[i + 1]) << 8;
        if (have > 2)
            group |= u32(in[i + 2]);

        out[at++] = BASE64[group >> 18 & 63];
        out[at++] = BASE64[group >> 12 & 63];
        out[at++] = have > 1 ? BASE64[group >> 6 & 63] : '=';
        out[at++] = have > 2 ? BASE64[group & 63] : '=';
    }
    return true;
}

Option<usize> base64_decode(Str in, Span<u8> out)
{
    if (in.size() % 4 != 0)
        return None;
    if (in.empty())
        return usize(0);

    usize pad = 0;
    if (in[in.size() - 1] == '=')
        pad = in[in.size() - 2] == '=' ? 2 : 1;

    usize n = in.size() / 4 * 3 - pad;
    if (out.size() < n)
        return None;

    usize at = 0;
    for (usize i = 0; i < in.size(); i += 4) {
        bool last  = i + 4 == in.size();
        usize have = last ? 4 - pad : 4;
        u32 group  = 0;
        for (usize k = 0; k < have; k++) {
            u32 v = base64_value(in[i + k]);
            if (v == 64)
                return None;
            group |= v << (18 - 6 * k);
        }
        // The bits of the bytes this group does not yield must be zero, or two
        // spellings decode alike.
        if (group & ((1u << (8 * (4 - have))) - 1))
            return None;

        out[at++] = u8(group >> 16);
        if (have > 2)
            out[at++] = u8(group >> 8);
        if (have > 3)
            out[at++] = u8(group);
    }
    return n;
}

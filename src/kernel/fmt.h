// A fixed-capacity text builder. Overflow truncates and sets a flag rather
// than allocating, so it is safe on the panic path.
#pragma once

#include "str.h"
#include "types.h"

template <usize N>
struct Buf {
    Buf &put(Str s)
    {
        for (usize i = 0; i < s.size(); i++)
            put(s[i]);
        return *this;
    }

    Buf &put(char c)
    {
        if (n_ < N)
            b_[n_++] = c;
        else
            full_ = true;
        return *this;
    }

    Buf &put(u32 v)
    {
        char t[10];
        usize k = 0;
        do {
            t[k++] = char('0' + v % 10);
            v /= 10;
        } while (v);
        while (k)
            put(t[--k]);
        return *this;
    }

    Buf &put(usize v) { return put(u32(v)); }

    // A storage quota does not fit in 32 bits.
    Buf &put(u64 v)
    {
        char t[20];
        usize k = 0;
        do {
            t[k++] = char('0' + u32(v % 10));
            v /= 10;
        } while (v);
        while (k)
            put(t[--k]);
        return *this;
    }

    Buf &put(int v)
    {
        if (v < 0) {
            put('-');
            return put(u32(-(v + 1)) + 1);
        }
        return put(u32(v));
    }

    Buf &put_hex(u32 v)
    {
        put("0x");
        for (int shift = 28; shift >= 0; shift -= 4)
            put("0123456789abcdef"[(v >> shift) & 0xF]);
        return *this;
    }

    Str str() const { return Str(b_, n_); }

    bool overflowed() const { return full_; }

    void clear()
    {
        n_    = 0;
        full_ = false;
    }

private:
    char b_[N];
    usize n_   = 0;
    bool full_ = false;
};

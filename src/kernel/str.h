// Str — a non-owning view of UTF-8 bytes. Never null-terminated.
#pragma once

#include "types.h"

struct Str {
    constexpr Str() = default;

    constexpr Str(const char *p, usize n) : p_(p), n_(n) {}

    // Literals and null-terminated C strings; the terminator is not counted.
    constexpr Str(const char *s) : p_(s), n_(s ? __builtin_strlen(s) : 0) {}

    constexpr const char *data() const { return p_; }

    constexpr usize size() const { return n_; }

    constexpr bool empty() const { return n_ == 0; }

    constexpr char operator[](usize i) const { return p_[i]; }

    constexpr const char *begin() const { return p_; }

    constexpr const char *end() const { return p_ + n_; }

    constexpr Str substr(usize pos, usize n = ~usize(0)) const {
        if (pos > n_)
            pos = n_;
        usize avail = n_ - pos;
        return Str(p_ + pos, n < avail ? n : avail);
    }

    constexpr bool starts_with(Str s) const { return n_ >= s.n_ && equal(p_, s.p_, s.n_); }

    constexpr bool ends_with(Str s) const {
        return n_ >= s.n_ && equal(p_ + n_ - s.n_, s.p_, s.n_);
    }

    // Index of the first occurrence, or npos.
    static constexpr usize npos = ~usize(0);

    constexpr usize find(char c, usize from = 0) const {
        for (usize i = from; i < n_; i++)
            if (p_[i] == c)
                return i;
        return npos;
    }

    constexpr usize find(Str s, usize from = 0) const {
        if (s.n_ > n_)
            return npos;
        for (usize i = from; i + s.n_ <= n_; i++)
            if (equal(p_ + i, s.p_, s.n_))
                return i;
        return npos;
    }

    constexpr bool contains(Str s) const { return find(s) != npos; }

    // Splits at the first `sep`, returning the head and assigning the tail to
    // `rest`. With no separator the whole string is the head and `rest` empties.
    // `rest` may alias *this, which is what makes a tokenising loop read well.
    constexpr Str split(char sep, Str &rest) const {
        usize i = find(sep);
        Str head = i == npos ? *this : Str(p_, i);
        rest = i == npos ? Str(p_ + n_, 0) : Str(p_ + i + 1, n_ - i - 1);
        return head;
    }

    friend constexpr bool operator==(Str a, Str b) {
        return a.n_ == b.n_ && equal(a.p_, b.p_, a.n_);
    }

    friend constexpr bool operator!=(Str a, Str b) { return !(a == b); }

private:
    static constexpr bool equal(const char *a, const char *b, usize n) {
        for (usize i = 0; i < n; i++)
            if (a[i] != b[i])
                return false;
        return true;
    }

    const char *p_ = nullptr;
    usize n_ = 0;
};

constexpr Str operator""_s(const char *p, usize n) {
    return Str(p, n);
}

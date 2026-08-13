// Span<T> — a non-owning view of a contiguous range.
#pragma once

#include "types.h"

template <class T>
struct Span {
    constexpr Span() = default;

    constexpr Span(T *p, usize n) : p_(p), n_(n) {}

    template <usize N>
    constexpr Span(T (&a)[N]) : p_(a), n_(N)
    {
    }

    constexpr operator Span<const T>() const { return Span<const T>(p_, n_); }

    constexpr T *data() const { return p_; }

    constexpr usize size() const { return n_; }

    constexpr bool empty() const { return n_ == 0; }

    constexpr T &operator[](usize i) const { return p_[i]; }

    constexpr T *begin() const { return p_; }

    constexpr T *end() const { return p_ + n_; }

    constexpr Span subspan(usize pos, usize n = ~usize(0)) const
    {
        if (pos > n_)
            pos = n_;
        usize avail = n_ - pos;
        return Span(p_ + pos, n < avail ? n : avail);
    }

private:
    T *p_    = nullptr;
    usize n_ = 0;
};

using Bytes = Span<const u8>;

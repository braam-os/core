// String — an owning, growable, move-only UTF-8 buffer over the kernel heap.
// Str views it; nothing here is null-terminated.
#pragma once

#include "alloc.h"
#include "host.h"
#include "str.h"
#include "traits.h"
#include "types.h"

struct String {
    String() = default;

    String(String &&o) noexcept : p_(o.p_), n_(o.n_), cap_(o.cap_)
    {
        o.p_   = nullptr;
        o.n_   = 0;
        o.cap_ = 0;
    }

    String &operator=(String &&o) noexcept
    {
        if (this != &o) {
            heap_free(p_);
            p_     = o.p_;
            n_     = o.n_;
            cap_   = o.cap_;
            o.p_   = nullptr;
            o.n_   = 0;
            o.cap_ = 0;
        }
        return *this;
    }

    String(const String &)            = delete;
    String &operator=(const String &) = delete;

    ~String() { heap_free(p_); }

    usize size() const { return n_; }

    usize capacity() const { return cap_; }

    bool empty() const { return n_ == 0; }

    char *data() { return p_; }

    const char *data() const { return p_; }

    char operator[](usize i) const { return p_[i]; }

    char &operator[](usize i) { return p_[i]; }

    const char *begin() const { return p_; }

    const char *end() const { return p_ + n_; }

    Str str() const { return Str(p_, n_); }

    operator Str() const { return str(); }

    bool reserve(usize want)
    {
        if (want <= cap_)
            return true;
        usize cap = cap_ ? cap_ : 16;
        while (cap < want)
            cap *= 2;

        char *q = static_cast<char *>(heap_alloc(cap));
        if (!q)
            return false;
        __builtin_memcpy(q, p_, n_);
        heap_free(p_);
        p_   = q;
        cap_ = cap;
        return true;
    }

    bool push(char c)
    {
        if (n_ == cap_ && !reserve(n_ + 1))
            return false;
        p_[n_++] = c;
        return true;
    }

    bool append(Str s)
    {
        if (!reserve(n_ + s.size()))
            return false;
        __builtin_memcpy(p_ + n_, s.data(), s.size());
        n_ += s.size();
        return true;
    }

    bool assign(Str s)
    {
        n_ = 0;
        return append(s);
    }

    void pop()
    {
        if (n_ == 0)
            panic("String::pop on an empty string");
        n_--;
    }

    // Splices `s` in at `at`, which is clamped to the end. In place, because
    // an editor does this on every keystroke and a rebuild would churn the
    // heap for a line that is nearly always shorter than its capacity.
    bool insert(usize at, Str s)
    {
        if (at > n_)
            at = n_;
        if (!reserve(n_ + s.size()))
            return false;
        for (usize i = n_; i > at; i--)
            p_[i - 1 + s.size()] = p_[i - 1];
        __builtin_memcpy(p_ + at, s.data(), s.size());
        n_ += s.size();
        return true;
    }

    // Removes up to `n` bytes at `at`. Out of range is a no-op.
    void erase(usize at, usize n = 1)
    {
        if (at >= n_)
            return;
        if (n > n_ - at)
            n = n_ - at;
        for (usize i = at; i + n < n_; i++)
            p_[i] = p_[i + n];
        n_ -= n;
    }

    void clear() { n_ = 0; }

    // Drops everything past `n`. Never grows, and never touches the capacity.
    void truncate(usize n)
    {
        if (n < n_)
            n_ = n;
    }

    friend bool operator==(const String &a, Str b) { return a.str() == b; }

    friend bool operator!=(const String &a, Str b) { return !(a.str() == b); }

private:
    char *p_   = nullptr;
    usize n_   = 0;
    usize cap_ = 0;
};

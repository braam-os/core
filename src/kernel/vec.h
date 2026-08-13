// Vec<T> — a growable, move-only array over the kernel heap.
#pragma once

#include "alloc.h"
#include "host.h"
#include "span.h"
#include "traits.h"
#include "types.h"

template <class T>
struct Vec {
    Vec() = default;

    Vec(Vec &&o) noexcept : p_(o.p_), n_(o.n_), cap_(o.cap_)
    {
        o.p_   = nullptr;
        o.n_   = 0;
        o.cap_ = 0;
    }

    Vec &operator=(Vec &&o) noexcept
    {
        if (this != &o) {
            release();
            p_     = o.p_;
            n_     = o.n_;
            cap_   = o.cap_;
            o.p_   = nullptr;
            o.n_   = 0;
            o.cap_ = 0;
        }
        return *this;
    }

    Vec(const Vec &)            = delete;
    Vec &operator=(const Vec &) = delete;

    ~Vec() { release(); }

    usize size() const { return n_; }

    usize capacity() const { return cap_; }

    bool empty() const { return n_ == 0; }

    T *data() { return p_; }

    const T *data() const { return p_; }

    T &operator[](usize i) { return p_[i]; }

    const T &operator[](usize i) const { return p_[i]; }

    T *begin() { return p_; }

    T *end() { return p_ + n_; }

    const T *begin() const { return p_; }

    const T *end() const { return p_ + n_; }

    T &back() { return p_[n_ - 1]; }

    operator Span<T>() { return Span<T>(p_, n_); }

    operator Span<const T>() const { return Span<const T>(p_, n_); }

    bool reserve(usize want)
    {
        if (want <= cap_)
            return true;
        usize cap = cap_ ? cap_ : 4;
        while (cap < want)
            cap *= 2;

        T *q = static_cast<T *>(heap_alloc(cap * sizeof(T)));
        if (!q)
            return false;
        for (usize i = 0; i < n_; i++) {
            new (q + i) T(move(p_[i]));
            p_[i].~T();
        }
        heap_free(p_);
        p_   = q;
        cap_ = cap;
        return true;
    }

    bool push(T v)
    {
        if (n_ == cap_ && !reserve(n_ + 1))
            return false;
        new (p_ + n_) T(move(v));
        n_++;
        return true;
    }

    template <class... A>
    bool emplace(A &&...a)
    {
        if (n_ == cap_ && !reserve(n_ + 1))
            return false;
        new (p_ + n_) T(forward<A>(a)...);
        n_++;
        return true;
    }

    void pop()
    {
        if (n_ == 0)
            panic("Vec::pop on an empty vector");
        n_--;
        p_[n_].~T();
    }

    // Shifts the tail up by one. i == size() appends.
    bool insert(usize i, T v)
    {
        if (i > n_)
            panic("Vec::insert past the end");
        if (n_ == cap_ && !reserve(n_ + 1))
            return false;
        if (i == n_) {
            new (p_ + n_) T(move(v));
        } else {
            new (p_ + n_) T(move(p_[n_ - 1]));
            for (usize k = n_ - 1; k > i; k--)
                p_[k] = move(p_[k - 1]);
            p_[i] = move(v);
        }
        n_++;
        return true;
    }

    // Removes n elements at i, shifting the tail down. n is clamped to the end.
    void erase(usize i, usize n = 1)
    {
        if (i > n_)
            panic("Vec::erase past the end");
        if (n > n_ - i)
            n = n_ - i;
        for (usize k = i; k + n < n_; k++)
            p_[k] = move(p_[k + n]);
        destroy_from(n_ - n);
        n_ -= n;
    }

    bool resize(usize n)
    {
        if (n > n_) {
            if (!reserve(n))
                return false;
            for (usize i = n_; i < n; i++)
                new (p_ + i) T();
        } else {
            destroy_from(n);
        }
        n_ = n;
        return true;
    }

    void clear()
    {
        destroy_from(0);
        n_ = 0;
    }

private:
    void destroy_from(usize from)
    {
        if constexpr (!is_trivially_destructible<T>)
            for (usize i = from; i < n_; i++)
                p_[i].~T();
    }

    void release()
    {
        destroy_from(0);
        heap_free(p_);
        p_   = nullptr;
        n_   = 0;
        cap_ = 0;
    }

    T *p_      = nullptr;
    usize n_   = 0;
    usize cap_ = 0;
};

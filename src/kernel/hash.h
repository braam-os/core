// HashMap<K, V> — open addressing with linear probing over the kernel heap.
// K and V must be default-constructible; every key the kernel uses is.
#pragma once

#include "alloc.h"
#include "str.h"
#include "traits.h"
#include "types.h"

// murmur3's finalizer, for integer keys.
inline u32 hash_key(u32 x)
{
    x ^= x >> 16;
    x *= 0x85ebca6b;
    x ^= x >> 13;
    x *= 0xc2b2ae35;
    x ^= x >> 16;
    return x;
}

// FNV-1a, for string keys.
inline u32 hash_key(Str s)
{
    u32 h = 2166136261u;
    for (usize i = 0; i < s.size(); i++) {
        h ^= u8(s[i]);
        h *= 16777619u;
    }
    return h;
}

template <class K, class V>
struct HashMap {
    HashMap() = default;

    HashMap(HashMap &&o) noexcept : s_(o.s_), cap_(o.cap_), n_(o.n_), used_(o.used_)
    {
        o.s_    = nullptr;
        o.cap_  = 0;
        o.n_    = 0;
        o.used_ = 0;
    }

    HashMap &operator=(HashMap &&o) noexcept
    {
        if (this != &o) {
            release();
            s_      = o.s_;
            cap_    = o.cap_;
            n_      = o.n_;
            used_   = o.used_;
            o.s_    = nullptr;
            o.cap_  = 0;
            o.n_    = 0;
            o.used_ = 0;
        }
        return *this;
    }

    HashMap(const HashMap &)            = delete;
    HashMap &operator=(const HashMap &) = delete;

    ~HashMap() { release(); }

    usize size() const { return n_; }

    usize capacity() const { return cap_; }

    bool empty() const { return n_ == 0; }

    V *find(K k)
    {
        Slot *s = slot_of(k);
        return s ? &s->v : nullptr;
    }

    const V *find(K k) const
    {
        const Slot *s = const_cast<HashMap *>(this)->slot_of(k);
        return s ? &s->v : nullptr;
    }

    bool contains(K k) { return slot_of(k) != nullptr; }

    // Replaces the value if the key is already present. False means OOM.
    bool insert(K k, V v)
    {
        if ((used_ + 1) * 4 >= cap_ * 3 && !grow(cap_ ? cap_ * 2 : 8))
            return false;

        usize mask = cap_ - 1;
        usize i    = hash_key(k) & mask;
        usize dead = cap_;
        for (usize step = 0; step < cap_; step++) {
            Slot &s = s_[i];
            if (s.state == FULL && s.k == k) {
                s.v = move(v);
                return true;
            }
            if (s.state == DEAD && dead == cap_)
                dead = i;
            if (s.state == EMPTY) {
                usize at = dead == cap_ ? i : dead;
                if (dead == cap_)
                    used_++;
                s_[at].k     = move(k);
                s_[at].v     = move(v);
                s_[at].state = FULL;
                n_++;
                return true;
            }
            i = (i + 1) & mask;
        }
        return false;
    }

    bool remove(K k)
    {
        Slot *s = slot_of(k);
        if (!s)
            return false;
        s->k     = K();
        s->v     = V();
        s->state = DEAD;
        n_--;
        return true;
    }

    void clear()
    {
        for (usize i = 0; i < cap_; i++) {
            s_[i].k     = K();
            s_[i].v     = V();
            s_[i].state = EMPTY;
        }
        n_    = 0;
        used_ = 0;
    }

private:
    enum : u8 { EMPTY = 0, FULL = 1, DEAD = 2 };

    struct Slot {
        K k{};
        V v{};
        u8 state = EMPTY;
    };

    Slot *slot_of(K k)
    {
        if (!cap_)
            return nullptr;
        usize mask = cap_ - 1;
        usize i    = hash_key(k) & mask;
        for (usize step = 0; step < cap_; step++) {
            Slot &s = s_[i];
            if (s.state == EMPTY)
                return nullptr;
            if (s.state == FULL && s.k == k)
                return &s;
            i = (i + 1) & mask;
        }
        return nullptr;
    }

    // Insert into a table known to have room and no matching key.
    void place(K k, V v)
    {
        usize mask = cap_ - 1;
        usize i    = hash_key(k) & mask;
        while (s_[i].state == FULL)
            i = (i + 1) & mask;
        s_[i].k     = move(k);
        s_[i].v     = move(v);
        s_[i].state = FULL;
        n_++;
        used_++;
    }

    // Rehashing is also how tombstones are reclaimed.
    bool grow(usize cap)
    {
        Slot *s = static_cast<Slot *>(heap_alloc(cap * sizeof(Slot)));
        if (!s)
            return false;
        for (usize i = 0; i < cap; i++)
            new (s + i) Slot();

        Slot *old     = s_;
        usize old_cap = cap_;
        s_            = s;
        cap_          = cap;
        n_            = 0;
        used_         = 0;
        for (usize i = 0; i < old_cap; i++) {
            if (old[i].state == FULL)
                place(move(old[i].k), move(old[i].v));
            old[i].~Slot();
        }
        heap_free(old);
        return true;
    }

    void release()
    {
        for (usize i = 0; i < cap_; i++)
            s_[i].~Slot();
        heap_free(s_);
        s_    = nullptr;
        cap_  = 0;
        n_    = 0;
        used_ = 0;
    }

    Slot *s_    = nullptr;
    usize cap_  = 0; // a power of two, or zero
    usize n_    = 0; // live entries
    usize used_ = 0; // live entries plus tombstones
};

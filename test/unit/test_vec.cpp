#include "harness.h"

#include "kernel/alloc.h"
#include "kernel/span.h"
#include "kernel/vec.h"

namespace {

int live_count;

struct Tracked {
    u32 v = 0;

    Tracked() { live_count++; }

    explicit Tracked(u32 x) : v(x) { live_count++; }

    Tracked(const Tracked &o) : v(o.v) { live_count++; }

    Tracked(Tracked &&o) noexcept : v(o.v) { live_count++; }

    Tracked &operator=(const Tracked &) = default;

    ~Tracked() { live_count--; }
};
} // namespace

void test_vec() {
    test_begin("vec");

    Vec<u32> v;
    CHECK(v.empty());
    CHECK_EQ(v.size(), 0);

    for (u32 i = 0; i < 1000; i++)
        v.push(i * 3);
    CHECK_EQ(v.size(), 1000);
    CHECK(v.capacity() >= 1000);
    CHECK_EQ(v[0], 0);
    CHECK_EQ(v[999], 2997);
    CHECK_EQ(v.back(), 2997);

    u32 sum = 0;
    for (u32 x : v)
        sum += x;
    CHECK_EQ(sum, 999 * 1000 / 2 * 3);

    v.pop();
    CHECK_EQ(v.size(), 999);

    v.resize(4);
    CHECK_EQ(v.size(), 4);
    CHECK_EQ(v[3], 9);
    v.resize(6);
    CHECK_EQ(v.size(), 6);
    CHECK_EQ(v[5], 0); // value-initialised

    Span<u32> sp = v;
    CHECK_EQ(sp.size(), 6);
    CHECK_EQ(sp[3], 9);
    CHECK_EQ(sp.subspan(2, 2).size(), 2);
    CHECK_EQ(sp.subspan(2, 2)[0], 6);

    // Move leaves the source empty and does not double-free.
    Vec<u32> w = move(v);
    CHECK_EQ(w.size(), 6);
    CHECK_EQ(v.size(), 0);
    CHECK(v.data() == nullptr);

    v = move(w);
    CHECK_EQ(v.size(), 6);
    v.clear();
    CHECK(v.empty());

    // insert and erase, which the line editor's buffer is built on.
    v.clear();
    for (u32 i = 0; i < 4; i++)
        v.push(i); // 0 1 2 3
    v.insert(0, 9);
    CHECK_EQ(v.size(), 5);
    CHECK_EQ(v[0], 9);
    CHECK_EQ(v[4], 3);
    v.insert(v.size(), 7);
    CHECK_EQ(v.back(), 7);
    v.insert(3, 8); // 9 0 1 8 2 3 7
    CHECK_EQ(v[3], 8);
    CHECK_EQ(v[4], 2);
    CHECK_EQ(v.size(), 7);

    v.erase(3);
    CHECK_EQ(v.size(), 6); // 9 0 1 2 3 7
    CHECK_EQ(v[3], 2);
    v.erase(1, 3);
    CHECK_EQ(v.size(), 3); // 9 3 7
    CHECK_EQ(v[0], 9);
    CHECK_EQ(v[1], 3);
    CHECK_EQ(v[2], 7);
    v.erase(1, 99); // clamped to the end
    CHECK_EQ(v.size(), 1);
    v.erase(0, 0);
    CHECK_EQ(v.size(), 1);
    v.erase(1); // at the end: a no-op
    CHECK_EQ(v.size(), 1);

    // Non-trivial elements are destroyed exactly once, including on regrowth.
    live_count = 0;
    {
        Vec<Tracked> t;
        for (u32 i = 0; i < 100; i++)
            t.emplace(i);
        CHECK_EQ(live_count, 100);
        CHECK_EQ(t[99].v, 99);
        t.pop();
        CHECK_EQ(live_count, 99);
        t.resize(10);
        CHECK_EQ(live_count, 10);
        t.erase(2, 3);
        CHECK_EQ(live_count, 7);
        CHECK_EQ(t[2].v, 5);
        t.insert(0, Tracked(42));
        CHECK_EQ(live_count, 8);
        CHECK_EQ(t[0].v, 42);
        CHECK_EQ(t[1].v, 0);
    }
    CHECK_EQ(live_count, 0);

    // The vector's own storage is returned to the heap.
    usize before = heap_stats().bytes_in_use;
    {
        Vec<u32> scratch;
        for (u32 i = 0; i < 50; i++)
            scratch.push(i);
    }
    CHECK_EQ(heap_stats().bytes_in_use, before);
}

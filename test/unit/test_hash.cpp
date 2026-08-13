#include "harness.h"
#include "kernel/hash.h"
#include "kernel/str.h"

void test_hash()
{
    test_begin("hash");

    HashMap<u32, u32> m;
    CHECK(m.empty());
    CHECK(m.find(1) == nullptr);
    CHECK(!m.remove(1));

    CHECK(m.insert(1, 10));
    CHECK(m.insert(2, 20));
    CHECK_EQ(m.size(), 2);
    CHECK(m.find(1) != nullptr);
    CHECK_EQ(*m.find(1), 10);
    CHECK_EQ(*m.find(2), 20);
    CHECK(m.find(3) == nullptr);

    // Inserting a present key replaces the value rather than growing.
    CHECK(m.insert(1, 11));
    CHECK_EQ(m.size(), 2);
    CHECK_EQ(*m.find(1), 11);

    // Removal leaves a tombstone that must not break a probe past it.
    CHECK(m.remove(1));
    CHECK_EQ(m.size(), 1);
    CHECK(m.find(1) == nullptr);
    CHECK_EQ(*m.find(2), 20);
    CHECK(m.insert(1, 12));
    CHECK_EQ(*m.find(1), 12);

    // Growth rehashes: every key survives across several table doublings.
    for (u32 i = 0; i < 200; i++)
        CHECK(m.insert(i + 100, i));
    CHECK_EQ(m.size(), 202);
    CHECK(m.capacity() >= 256);
    bool all = true;
    for (u32 i = 0; i < 200; i++) {
        u32 *v = m.find(i + 100);
        if (!v || *v != i)
            all = false;
    }
    CHECK(all);
    CHECK_EQ(*m.find(2), 20);

    m.clear();
    CHECK(m.empty());
    CHECK(m.find(2) == nullptr);
    CHECK(m.insert(2, 21));
    CHECK_EQ(*m.find(2), 21);

    // Pointer values and string keys, the two shapes the kernel uses.
    HashMap<u32, u32 *> p;
    u32 target = 5;
    CHECK(p.insert(7, &target));
    CHECK(**p.find(7) == 5);

    HashMap<Str, u32> names;
    CHECK(names.insert("sleep", 1));
    CHECK(names.insert("wake", 2));
    CHECK_EQ(*names.find("wake"), 2);
    CHECK(names.find("tick") == nullptr);
}

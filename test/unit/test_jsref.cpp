#include "harness.h"
#include "kernel/jsref.h"
#include "kernel/traits.h"

// The table itself can only be checked from this side: jsref_get returns an
// externref, which has no comparison and cannot be stored in linear memory. So
// these cases are about slot bookkeeping — the part that leaks if it is wrong.

void test_jsref()
{
    test_begin("jsref");

    usize live = jsref_live();

    JsSlot a = jsref_reserve();
    JsSlot b = jsref_reserve();
    CHECK(a != 0);
    CHECK(b != 0);
    CHECK(a != b);
    CHECK_EQ(jsref_live(), live + 2);

    // A released slot comes back, so a shell that fetches a thousand URLs does
    // not grow the table a thousand entries.
    jsref_release(b);
    CHECK_EQ(jsref_live(), live + 1);
    CHECK_EQ(jsref_reserve(), b);

    jsref_release(a);
    jsref_release(b);
    CHECK_EQ(jsref_live(), live);

    // Slot 0 is null and is never handed out, so releasing it is a no-op
    // rather than an underflow.
    jsref_release(0);
    CHECK_EQ(jsref_live(), live);

    // Past the first chunk: the table has to grow, and the new slots are
    // distinct from every earlier one.
    JsSlot many[24];
    for (usize i = 0; i < 24; i++) {
        many[i] = jsref_reserve();
        CHECK(many[i] != 0);
        for (usize j = 0; j < i; j++)
            CHECK(many[i] != many[j]);
    }
    CHECK_EQ(jsref_live(), live + 24);
    for (usize i = 0; i < 24; i++)
        jsref_release(many[i]);
    CHECK_EQ(jsref_live(), live);

    // JsRef is the RAII form: the slot goes back in the destructor, and a move
    // leaves the source owning nothing.
    {
        JsRef r = JsRef::reserve();
        CHECK(r.ok());
        CHECK_EQ(jsref_live(), live + 1);

        JsSlot slot = r.slot();
        JsRef moved = move(r);
        CHECK(!r.ok());
        CHECK_EQ(moved.slot(), slot);
        CHECK_EQ(jsref_live(), live + 1);
    }
    CHECK_EQ(jsref_live(), live);

    // take() is how a slot survives its JsRef: nothing is released here, so
    // the count stays up until it is released by hand.
    JsSlot kept = 0;
    {
        JsRef r = JsRef::reserve();
        kept    = r.take();
        CHECK(!r.ok());
    }
    CHECK_EQ(jsref_live(), live + 1);
    jsref_release(kept);
    CHECK_EQ(jsref_live(), live);
}

#include "harness.h"
#include "kernel/alloc.h"
#include "kernel/screen.h"
#include "user/io.h"
#include "user/tty.h"

// The three routes through the tty pump, each with one holder. Destroying them
// out of the order they were made is the case that matters: a parent and a
// child claim in either order and die in either order, and the pump must be
// left pointing at nothing rather than at a freed ring or a dead pipe. The
// claims are reached directly here, since the in-wasm tests cannot run a
// program and only a program claims the screen.

void test_tty()
{
    test_begin("tty");

    // Raw keys: the second claimant is refused, and refusing it changes
    // nothing about the first.
    {
        KeyInput a(7);
        CHECK(a.ok());
        CHECK(tty_raw() != nullptr);
        CHECK_EQ(tty_keys_owner(), 7);

        KeyRing *held = tty_raw();
        {
            KeyInput b(9);
            CHECK(!b.ok());
            CHECK_EQ(b.error(), Error::Perm);
            CHECK(tty_raw() == held);
            CHECK_EQ(tty_keys_owner(), 7);
        }
        CHECK(tty_raw() == held);
        CHECK_EQ(tty_keys_owner(), 7);
    }
    CHECK(tty_raw() == nullptr);
    CHECK_EQ(tty_keys_owner(), 0);

    // And the route is claimable again once its holder is gone.
    {
        KeyInput c(9);
        CHECK(c.ok());
        CHECK_EQ(tty_keys_owner(), 9);
    }

    // Out of order, on the heap, which a scope cannot express: the refused
    // claim outlives the holder. Restoring a saved predecessor here left the
    // pump with a pointer to a freed ring.
    {
        KeyInput *a = heap_new<KeyInput>(1);
        KeyInput *b = heap_new<KeyInput>(2);
        CHECK(a && a->ok());
        CHECK(b && !b->ok());
        heap_delete(a);
        CHECK(tty_raw() == nullptr);
        heap_delete(b);
        CHECK(tty_raw() == nullptr);
        CHECK_EQ(tty_keys_owner(), 0);
    }

    // The screen: the second claimant must not snapshot the blanked grid the
    // first is painting, or the shell's screen is what gets thrown away.
    screen_reset();
    screen_resize(4, 3);
    screen_write("ab");
    {
        FullScreen *a = heap_new<FullScreen>(3);
        CHECK(a && a->ok());
        CHECK_EQ(tty_screen_owner(), 3);
        CHECK_EQ(screen_cells()[0].ch, 0); // taken, so blank

        FullScreen *b = heap_new<FullScreen>(4);
        CHECK(b && !b->ok());
        CHECK_EQ(b->error(), Error::Perm);
        CHECK_EQ(tty_screen_owner(), 3);

        heap_delete(a); // the holder first, then the refused one
        CHECK_EQ(tty_screen_owner(), 0);
        CHECK_EQ(screen_cells()[0].ch, 'a');
        heap_delete(b);
        CHECK_EQ(screen_cells()[0].ch, 'a');
    }
    screen_reset();

    // Cooked bytes: one holder, and the refused claim leaves the route alone
    // however late it is destroyed.
    {
        Pipe one, two;
        InputClaim *a = heap_new<InputClaim>(&one);
        InputClaim *b = heap_new<InputClaim>(&two);
        CHECK(a && a->ok());
        CHECK(b && !b->ok());
        CHECK(tty_cooked() == &one);
        heap_delete(a);
        CHECK(tty_cooked() == nullptr);
        heap_delete(b);
        CHECK(tty_cooked() == nullptr);
    }

    // A null pipe is a legal claim — `fg` makes one when the job it adopted has
    // no stage left — and it still holds the route against a second claimant.
    {
        InputClaim a(nullptr);
        CHECK(a.ok());
        CHECK(tty_cooked() == nullptr);
        InputClaim b(nullptr);
        CHECK(!b.ok());
    }
}

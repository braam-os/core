#include "harness.h"
#include "kernel/alloc.h"
#include "kernel/screen.h"
#include "user/console.h"
#include "user/io.h"
#include "user/tty.h"

// The two routes through the console pump, each with one holder. Destroying
// them out of the order they were made is the case that matters: a parent and a
// child claim in either order and die in either order, and the pump must be
// left pointing at nothing rather than at a freed ring. The claims are reached
// directly here, since the in-wasm tests cannot run a program and only a
// program claims either of them.

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

    // A view is taken down before the snapshot: a background job claims the
    // screen with no keystroke to have brought it home.
    screen_reset();
    screen_resize(4, 2);
    screen_write("ab\ncd\nef");
    CHECK(screen_view_scroll(-1) != 0);
    {
        FullScreen *a = heap_new<FullScreen>(5);
        CHECK(a && a->ok());
        CHECK_EQ(screen_view(), 0u);
        heap_delete(a);
        CHECK_EQ(screen_cells()[0].ch, 'c');
    }
    screen_reset();

    // What Sys::Tty answers with: a console stream and a pipe's are the same
    // type, so only the sink tells them apart.
    {
        Stdio c = stdio_console();
        CHECK(tty_is_console(c.out));
        CHECK(tty_is_console(c.err));
        CHECK(console_is_input(c.in));
        CHECK(!console_is_input(null_source()));

        Pipe *q = heap_new<Pipe>();
        CHECK(q != nullptr);
        if (q) {
            CHECK(!tty_is_console(pipe_sink(*q)));
            CHECK(!console_is_input(pipe_source(*q)));
            heap_delete(q);
        }
    }
}

#include "harness.h"
#include "kernel/alloc.h"
#include "kernel/screen.h"
#include "kernel/str.h"

namespace {

char32_t at(u32 x, u32 y)
{
    return screen_cells()[y * screen().cols + x].ch;
}

// What the renderer paints, which is the view's cells while one is up.
char32_t shown(u32 x, u32 y)
{
    const Cell *cells = reinterpret_cast<const Cell *>(usize(screen().cells));
    return cells[y * screen().cols + x].ch;
}

} // namespace

void test_screen()
{
    test_begin("screen");

    const Screen &s = screen();

    // A fresh grid is blank, and the descriptor describes it.
    screen_reset();
    CHECK(screen_resize(4, 3) != 0);
    CHECK_EQ(s.magic, SCREEN_MAGIC);
    CHECK_EQ(s.cols, 4);
    CHECK_EQ(s.rows, 3);
    CHECK_EQ(s.cursor_x, 0);
    CHECK_EQ(s.cursor_y, 0);
    CHECK(s.cells != 0);
    CHECK_EQ(at(0, 0), 0);
    CHECK_EQ(at(3, 2), 0);

    // The descriptor is a link-time constant, so its address never moves.
    CHECK_EQ(screen_resize(8, 4), screen_resize(4, 3));

    // Writing advances the cursor and damages exactly what it wrote.
    screen_reset();
    screen_resize(4, 3);
    screen_flush();
    screen_write("ab");
    CHECK_EQ(at(0, 0), 'a');
    CHECK_EQ(at(1, 0), 'b');
    CHECK_EQ(s.cursor_x, 2);
    Rect d = screen_damage();
    CHECK_EQ(d.x, 0);
    CHECK_EQ(d.y, 0);
    CHECK_EQ(d.w, 2);
    CHECK_EQ(d.h, 1);
    screen_flush();
    CHECK_EQ(screen_damage().w, 0);

    // The wrap is deferred: the cursor parks past the last column, and only
    // the next character moves it down.
    screen_reset();
    screen_resize(4, 3);
    screen_write("abcd");
    CHECK_EQ(s.cursor_x, 4);
    CHECK_EQ(s.cursor_y, 0);
    screen_put('e');
    CHECK_EQ(s.cursor_x, 1);
    CHECK_EQ(s.cursor_y, 1);
    CHECK_EQ(at(0, 1), 'e');

    // Past the last row the screen scrolls, losing the top.
    screen_reset();
    screen_resize(2, 2);
    screen_write("ab\ncd\n");
    CHECK_EQ(at(0, 0), 'c');
    CHECK_EQ(at(1, 0), 'd');
    CHECK_EQ(at(0, 1), 0);
    CHECK_EQ(s.cursor_x, 0);
    CHECK_EQ(s.cursor_y, 1);

    // Rows off the top are counted, which is the only way a writer holding an
    // anchor row learns that the anchor moved (Sys::Echo, Concept.md §4.3).
    // Nothing else in the grid records a scroll: cursor_y does not change.
    screen_reset();
    screen_resize(2, 2);
    CHECK_EQ(screen_scrolled(), u64(0));
    screen_write("ab\ncd");
    CHECK_EQ(screen_scrolled(), u64(0)); // filled it, nothing dropped yet
    screen_write("\nef");
    CHECK_EQ(screen_scrolled(), u64(1));
    CHECK_EQ(s.cursor_y, 1);
    screen_write("\ngh\nij");
    CHECK_EQ(screen_scrolled(), u64(3));

    // A resize that drops rows from the top is the grid moving up in exactly
    // the same sense, so it counts the same way.
    screen_reset();
    screen_resize(4, 4);
    screen_write("one\ntwo\nsix\nfor\n"); // the trailing newline scrolls once
    CHECK_EQ(screen_scrolled(), u64(1));
    CHECK(screen_resize(4, 2) != 0); // two of the four rows in use go
    CHECK_EQ(screen_scrolled(), u64(3));

    // Backspace erases, and walks back over a row boundary.
    screen_reset();
    screen_resize(3, 2);
    screen_write("abc");
    screen_put('d');
    CHECK_EQ(s.cursor_y, 1);
    screen_backspace();
    CHECK_EQ(s.cursor_x, 0);
    CHECK_EQ(s.cursor_y, 1);
    CHECK_EQ(at(0, 1), 0);
    screen_backspace();
    CHECK_EQ(s.cursor_x, 2);
    CHECK_EQ(s.cursor_y, 0);
    CHECK_EQ(at(2, 0), 0);
    screen_move(0, 0);
    screen_backspace(); // at the origin there is nothing to erase
    CHECK_EQ(s.cursor_x, 0);
    CHECK_EQ(s.cursor_y, 0);
    CHECK_EQ(at(0, 0), 'a');

    // Shrinking drops from the top of the rows in use, and carries the cursor.
    screen_reset();
    screen_resize(4, 4);
    screen_write("one\ntwo\nsix\nfor\n");
    CHECK_EQ(s.cursor_y, 3);
    CHECK(screen_resize(4, 2) != 0);
    CHECK_EQ(s.cols, 4);
    CHECK_EQ(s.rows, 2);
    CHECK_EQ(at(0, 0), 'f');
    CHECK_EQ(at(2, 0), 'r');
    CHECK_EQ(at(0, 1), 0);
    CHECK_EQ(s.cursor_x, 0);
    CHECK_EQ(s.cursor_y, 1);

    // Growing keeps everything where it was; output grows down into the space.
    CHECK(screen_resize(4, 4) != 0);
    CHECK_EQ(at(0, 0), 'f');
    CHECK_EQ(at(0, 2), 0);
    CHECK_EQ(s.cursor_y, 1);

    // Narrowing truncates columns.
    CHECK(screen_resize(2, 4) != 0);
    CHECK_EQ(at(0, 0), 'f');
    CHECK_EQ(at(1, 0), 'o');
    CHECK_EQ(s.cursor_y, 1);

    // Text above the cursor survives a shrink that the bottom-most rows would
    // have thrown away: a banner on row 0 of a tall, near-empty screen.
    screen_reset();
    screen_resize(8, 24);
    screen_write("banner\n");
    CHECK_EQ(s.cursor_y, 1);
    CHECK(screen_resize(8, 5) != 0);
    CHECK_EQ(at(0, 0), 'b');
    CHECK_EQ(s.cursor_y, 1);

    // A geometry the host has no business asking for is clamped, not honoured,
    // so the size computation cannot overflow.
    screen_reset();
    CHECK(screen_resize(9999, 9999) != 0);
    CHECK_EQ(s.cols, SCREEN_MAX_COLS);
    CHECK_EQ(s.rows, SCREEN_MAX_ROWS);
    CHECK(screen_resize(0, 0) != 0);
    CHECK_EQ(s.cols, 1);
    CHECK_EQ(s.rows, 1);

    // Clearing blanks the grid and homes the cursor.
    screen_reset();
    screen_resize(3, 2);
    screen_write("ab\ncd");
    screen_clear();
    CHECK_EQ(at(0, 0), 0);
    CHECK_EQ(at(1, 1), 0);
    CHECK_EQ(s.cursor_x, 0);
    CHECK_EQ(s.cursor_y, 0);

    // Nothing is kept until something scrolls: a screen that never fills pays
    // nothing for the ring.
    screen_reset();
    screen_resize(4, 3);
    CHECK_EQ(screen_history(), 0u);
    CHECK_EQ(screen_view(), 0u);
    screen_write("a\nb\nc"); // fills it exactly, so nothing has left
    CHECK_EQ(screen_history(), 0u);
    CHECK_EQ(screen_view_scroll(-1), 0u); // and there is nowhere to go
    screen_write("\nd");
    CHECK_EQ(screen_history(), 1u);

    // Paging back shows what left, over a live grid that has not moved.
    screen_cursor(true);
    CHECK_EQ(screen_view_scroll(-1), 1u);
    CHECK_EQ(screen_view(), 1u);
    CHECK_EQ(shown(0, 0), 'a'); // out of the ring
    CHECK_EQ(shown(0, 1), 'b'); // and the live rows below it
    CHECK_EQ(shown(0, 3 - 1), 'c');
    CHECK_EQ(at(0, 0), 'b'); // the grid itself is untouched
    CHECK_EQ(s.cursor_on, 0u);
    CHECK(screen_cursor_on()); // hidden, not turned off

    // Home restores both, and clamps hold at either end.
    screen_view_home();
    CHECK_EQ(screen_view(), 0u);
    CHECK_EQ(shown(0, 0), 'b');
    CHECK_EQ(shown(0, 2), 'd');
    CHECK_EQ(s.cursor_on, 1u);
    CHECK_EQ(screen_view_scroll(-100), screen_history());
    CHECK_EQ(screen_view_scroll(100), 0u);

    // Output moves the live grid under a view; the view stays on its rows.
    screen_reset();
    screen_resize(4, 2);
    screen_write("a\nb\nc\nd");
    CHECK_EQ(screen_history(), 2u);
    CHECK_EQ(screen_view_scroll(-2), 2u);
    CHECK_EQ(shown(0, 0), 'a');
    u64 was = screen_scrolled();
    screen_write("\ne");
    CHECK_EQ(screen_scrolled(), was + 1); // the anchor count is unaffected
    CHECK_EQ(screen_view(), 3u);          // and the offset followed the rows
    screen_flush();                       // where the recompose happens
    CHECK_EQ(shown(0, 0), 'a');
    CHECK_EQ(at(0, 1), 'e'); // the live screen carried on regardless

    // Past the ring's depth the oldest row is gone, so the view drifts.
    screen_reset();
    screen_resize(2, 2);
    for (u32 i = 0; i < SCREEN_SCROLLBACK + 4; i++) {
        screen_put(char32_t('0' + i % 10));
        screen_newline();
    }
    CHECK_EQ(screen_history(), SCREEN_SCROLLBACK);
    CHECK_EQ(screen_view_scroll(-i32(SCREEN_SCROLLBACK)), SCREEN_SCROLLBACK);
    char32_t top = shown(0, 0);
    screen_newline();
    CHECK_EQ(screen_view(), SCREEN_SCROLLBACK); // clamped, not grown
    screen_flush();
    CHECK(shown(0, 0) != top);

    // Clearing the screen does not clear what has scrolled off it.
    screen_view_home();
    screen_clear();
    CHECK_EQ(screen_history(), SCREEN_SCROLLBACK);

    // Rows a resize drops off the top are history in the same sense.
    screen_reset();
    screen_resize(4, 4);
    screen_write("one\ntwo\nsix\nfor\n");
    CHECK_EQ(screen_history(), 1u);
    CHECK(screen_resize(4, 2) != 0);
    CHECK_EQ(screen_history(), 3u);
    CHECK_EQ(screen_view_scroll(-3), 3u);
    CHECK_EQ(shown(0, 0), 'o'); // "one", the oldest of them
    CHECK_EQ(shown(0, 1), 't'); // "two"

    // History follows a width change, clipped and padded like the grid.
    screen_reset();
    screen_resize(4, 2);
    screen_write("abcd\nefgh\nij");
    CHECK_EQ(screen_history(), 1u);
    CHECK(screen_resize(2, 2) != 0);
    CHECK_EQ(screen_view_scroll(-1), 1u);
    CHECK_EQ(shown(0, 0), 'a');
    CHECK_EQ(shown(1, 0), 'b'); // "abcd", to the two columns there are
    CHECK(screen_resize(4, 2) != 0);
    CHECK_EQ(screen_view(), 0u); // a resize takes the view down first
    CHECK_EQ(screen_view_scroll(-1), 1u);
    CHECK_EQ(shown(1, 0), 'b');
    CHECK_EQ(shown(2, 0), 0); // and pads the width back out blank

    // The grid, the ring and the view are the allocations, and reset gives all
    // three back.
    screen_reset();
    usize in_use = heap_stats().bytes_in_use;
    screen_resize(8, 2);
    screen_write("a\nb\nc\nd");
    CHECK(screen_history() != 0);
    CHECK(screen_view_scroll(-1) != 0);
    CHECK(heap_stats().bytes_in_use > in_use);
    screen_reset();
    CHECK_EQ(heap_stats().bytes_in_use, in_use);
}

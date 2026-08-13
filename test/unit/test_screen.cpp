#include "harness.h"

#include "kernel/alloc.h"
#include "kernel/screen.h"
#include "kernel/str.h"

namespace {

char32_t at(u32 x, u32 y) {
    return screen_cells()[y * screen().cols + x].ch;
}

} // namespace

void test_screen() {
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

    // The grid is the only allocation, and reset gives it back.
    screen_reset();
    usize in_use = heap_stats().bytes_in_use;
    screen_resize(80, 24);
    CHECK(heap_stats().bytes_in_use > in_use);
    screen_reset();
    CHECK_EQ(heap_stats().bytes_in_use, in_use);
}

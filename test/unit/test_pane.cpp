#include "harness.h"
#include "kernel/screen.h"
#include "ui/full.h"
#include "ui/pane.h"

namespace {

char32_t at(u32 x, u32 y)
{
    return screen_cells()[y * screen().cols + x].ch;
}

const Cell &cell(u32 x, u32 y)
{
    return screen_cells()[y * screen().cols + x];
}

} // namespace

void test_pane()
{
    test_begin("pane");

    screen_reset();
    CHECK(screen_resize(8, 4) != 0);

    // A pane writes in its own coordinates and damages what it wrote.
    screen_flush();
    Pane p(2, 1, 4, 2);
    CHECK_EQ(p.width(), 4);
    CHECK_EQ(p.height(), 2);
    p.write_at(0, 0, "ab");
    CHECK_EQ(at(2, 1), 'a');
    CHECK_EQ(at(3, 1), 'b');

    Rect d = screen_damage();
    CHECK_EQ(d.x, 2);
    CHECK_EQ(d.y, 1);
    CHECK_EQ(d.w, 2);
    CHECK_EQ(d.h, 1);

    // Writing past the right edge drops rather than wrapping: a pane is a
    // rectangle, and the row below belongs to somebody else.
    p.write_at(0, 1, "wxyz!!");
    CHECK_EQ(at(5, 2), 'z');
    CHECK_EQ(at(6, 2), 0);
    CHECK_EQ(at(0, 3), 0);

    // A row past the bottom is not a row.
    p.write_at(0, 9, "no");
    CHECK_EQ(at(2, 3), 0);

    // Style is the pane's own, and fill_row pads to the right edge with it.
    p.style(COLOR_RED, COLOR_BLUE, ATTR_BOLD);
    p.move(0, 0);
    p.put('z');
    p.fill_row();
    CHECK_EQ(cell(2, 1).fg, COLOR_RED);
    CHECK_EQ(cell(5, 1).bg, COLOR_BLUE);
    CHECK_EQ(cell(5, 1).ch, 0);           // blank, but still the pane's colours
    CHECK_EQ(cell(6, 1).bg, COLOR_BLACK); // outside the pane, untouched

    // A sub-pane is clipped to its parent, and one that starts outside it is
    // empty rather than out of bounds.
    Pane s = p.sub(2, 0, 8, 8);
    CHECK_EQ(s.width(), 2);
    CHECK_EQ(s.height(), 2);
    CHECK_EQ(p.sub(9, 0, 2, 2).width(), 0);
    s.write_at(0, 0, "QQ");
    CHECK_EQ(at(4, 1), 'Q');
    CHECK_EQ(at(5, 1), 'Q');

    // bottom() is what a status line is made of.
    Pane root = Pane::root();
    CHECK_EQ(root.width(), 8);
    Pane bar = root.bottom(1);
    bar.write_at(0, 0, "s");
    CHECK_EQ(at(0, 3), 's');

    // The cursor lands in pane coordinates, clamped to the pane.
    p.place_cursor(1, 0);
    CHECK_EQ(screen().cursor_x, 3);
    CHECK_EQ(screen().cursor_y, 1);
    p.place_cursor(99, 99);
    CHECK_EQ(screen().cursor_x, 5);
    CHECK_EQ(screen().cursor_y, 2);

    // A full screen is saved and given back, cell for cell.
    screen_reset();
    CHECK(screen_resize(4, 2) != 0);
    screen_write("hi");
    screen_move(1, 0);
    screen_cursor(true);
    {
        FullScreen fs;
        CHECK(fs.ok());
        CHECK_EQ(at(0, 0), 0); // blanked for the program that took it
        CHECK_EQ(screen().cursor_on, 0);
        CHECK_EQ(fs.body().height(), 1);
        CHECK_EQ(fs.status().height(), 1);
        fs.body().write_at(0, 0, "XY");
        CHECK_EQ(at(0, 0), 'X');
    }
    CHECK_EQ(at(0, 0), 'h');
    CHECK_EQ(at(1, 0), 'i');
    CHECK_EQ(screen().cursor_x, 1);
    CHECK_EQ(screen().cursor_on, 1);

    // A resize under a full-screen program throws the snapshot away rather
    // than restoring cells that describe a grid which no longer exists.
    {
        FullScreen fs;
        CHECK(fs.ok());
        CHECK(screen_resize(6, 3) != 0);
    }
    CHECK_EQ(at(0, 0), 0);
    CHECK_EQ(screen().cols, 6);

    screen_reset();
}

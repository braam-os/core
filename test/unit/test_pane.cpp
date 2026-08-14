#include "harness.h"
#include "kernel/alloc.h"
#include "ui/pane.h"

// A Pane writes into a Grid, and the Grid is the test's own: since the layout
// layer became a library both the kernel and a binary link (Concept.md §4.3),
// it has no screen to reach for, which is what made it linkable at all.

namespace {

Grid g;
Cell storage[8 * 4];

void grid(u32 cols, u32 rows)
{
    g = Grid{};
    __builtin_memset(storage, 0, sizeof(storage));
    g.cells = storage;
    g.cols  = cols;
    g.rows  = rows;
}

char32_t at(u32 x, u32 y)
{
    return g.cells[y * g.cols + x].ch;
}

const Cell &cell(u32 x, u32 y)
{
    return g.cells[y * g.cols + x];
}

} // namespace

void test_pane()
{
    test_begin("pane");

    grid(8, 4);

    // A pane writes in its own coordinates and damages what it wrote.
    Pane p(g, 2, 1, 4, 2);
    CHECK_EQ(p.width(), 4);
    CHECK_EQ(p.height(), 2);
    p.write_at(0, 0, "ab");
    CHECK_EQ(at(2, 1), 'a');
    CHECK_EQ(at(3, 1), 'b');

    Rect d = g.take_damage();
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

    // top() and bottom() are what a body and a status line are made of.
    Pane root = Pane::of(g);
    CHECK_EQ(root.width(), 8);
    CHECK_EQ(root.top(3).height(), 3);
    Pane bar = root.bottom(1);
    bar.write_at(0, 0, "s");
    CHECK_EQ(at(0, 3), 's');

    // The cursor lands in pane coordinates, clamped to the pane, and on the
    // grid rather than on a screen: who acts on it is the caller's business.
    p.place_cursor(1, 0);
    CHECK_EQ(g.cursor_x, 3);
    CHECK_EQ(g.cursor_y, 1);
    p.place_cursor(99, 99);
    CHECK_EQ(g.cursor_x, 5);
    CHECK_EQ(g.cursor_y, 2);

    // Damage accumulates as one rectangle over every write since the last
    // time it was taken, which is what a full repaint sends in one blit.
    grid(8, 4);
    Pane q = Pane::of(g);
    q.write_at(1, 1, "a");
    q.write_at(6, 3, "b");
    Rect all = g.take_damage();
    CHECK_EQ(all.x, 1);
    CHECK_EQ(all.y, 1);
    CHECK_EQ(all.w, 6);
    CHECK_EQ(all.h, 3);
    CHECK_EQ(g.take_damage().w, 0); // taken once

    // A pane with no grid behind it is empty rather than a null dereference.
    Pane none;
    none.write_at(0, 0, "x");
    none.place_cursor(0, 0);
    CHECK_EQ(none.width(), 0);
    CHECK_EQ(none.sub(0, 0, 1, 1).width(), 0);
}

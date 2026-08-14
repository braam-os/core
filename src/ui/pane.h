// A pane: a rectangle of the cell grid with its own coordinates, style and
// cursor (Concept.md §3.5). This is the whole of the layout layer's geometry —
// a program composes its screen out of panes, and every write is clipped to the
// one it goes through, so a status line cannot scribble on the text above it.
//
// A pane writes cells directly into a Grid and marks them damaged there. It
// never scrolls: scrolling moves the whole screen, which is exactly what a
// full-screen program must not do. A view that scrolls repaints instead.
//
// The Grid is a parameter rather than the kernel's screen, which is what lets
// this file link into a process binary as well as into the kernel: a program
// paints its own buffer and blits the damage across (Concept.md §4.3).
#pragma once

#include "grid.h"
#include "kernel/str.h"
#include "kernel/types.h"

struct Pane {
    Pane() = default;

    Pane(Grid &g, u32 x, u32 y, u32 w, u32 h) : g_(&g), x_(x), y_(y), w_(w), h_(h) {}

    // The whole of a grid. Empty before it has been sized.
    static Pane of(Grid &g) { return Pane{ g, 0, 0, g.cols, g.rows }; }

    u32 width() const { return w_; }

    u32 height() const { return h_; }

    // A sub-rectangle, in this pane's coordinates, clipped to it.
    Pane sub(u32 x, u32 y, u32 w, u32 h) const;

    // The rows above `n` from the bottom, and those bottom `n` rows.
    Pane top(u32 rows) const { return sub(0, 0, w_, rows); }

    Pane bottom(u32 rows) const;

    void style(u8 fg, u8 bg, u8 attrs = 0);

    // Where the next put() lands. Clamped, so a pane's cursor never leaves it.
    void move(u32 x, u32 y);

    u32 cursor_x() const { return cx_; }

    u32 cursor_y() const { return cy_; }

    // One codepoint at the cursor, which advances. A write past the right edge
    // is dropped rather than wrapped: a pane is a rectangle, not a stream.
    void put(char32_t ch);

    // UTF-8, decoded and put one codepoint at a time.
    void write(Str utf8);

    // Writes at (x, y) and leaves the cursor after it.
    void write_at(u32 x, u32 y, Str utf8);

    // Pads to the right edge with blanks in the current style.
    void fill_row();

    void clear();

    // Puts the real cursor here, in pane coordinates, so the renderer draws it.
    void place_cursor(u32 x, u32 y) const;

private:
    Cell *cell(u32 x, u32 y) const;

    Grid *g_ = nullptr;
    u32 x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    u32 cx_ = 0, cy_ = 0;
    u8 fg_    = COLOR_WHITE;
    u8 bg_    = COLOR_BLACK;
    u8 attrs_ = 0;
};

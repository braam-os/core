#include "pane.h"

#include "kernel/text.h"
#include "kernel/traits.h"

Pane Pane::root()
{
    const Screen &s = screen();
    return Pane{ 0, 0, s.cols, s.rows };
}

Pane Pane::sub(u32 x, u32 y, u32 w, u32 h) const
{
    if (x >= w_ || y >= h_)
        return Pane{ x_ + w_, y_ + h_, 0, 0 };
    return Pane{ x_ + x, y_ + y, min(w, w_ - x), min(h, h_ - y) };
}

Pane Pane::bottom(u32 rows) const
{
    u32 n = min(rows, h_);
    return sub(0, h_ - n, w_, n);
}

void Pane::style(u8 fg, u8 bg, u8 attrs)
{
    fg_    = fg;
    bg_    = bg;
    attrs_ = attrs;
}

void Pane::move(u32 x, u32 y)
{
    cx_ = w_ ? min(x, w_ - 1) : 0;
    cy_ = h_ ? min(y, h_ - 1) : 0;
}

Cell *Pane::cell(u32 x, u32 y) const
{
    Cell *cells = screen_cells();
    if (!cells || x >= w_ || y >= h_)
        return nullptr;
    return cells + (y_ + y) * screen().cols + (x_ + x);
}

void Pane::put(char32_t ch)
{
    if (cx_ >= w_)
        return;
    if (Cell *c = cell(cx_, cy_)) {
        *c = Cell{ ch, fg_, bg_, attrs_, 0 };
        screen_touch(x_ + cx_, y_ + cy_, 1, 1);
    }
    cx_++;
}

void Pane::write(Str utf8)
{
    usize i = 0;
    while (i < utf8.size()) {
        char32_t ch;
        usize len = utf8_decode(utf8, i, ch);
        if (!len)
            return;
        i += len;
        if (ch == '\n') // a pane has no line discipline; show it as a space
            ch = ' ';
        put(ch);
    }
}

void Pane::write_at(u32 x, u32 y, Str utf8)
{
    if (x >= w_ || y >= h_)
        return;
    cx_ = x;
    cy_ = y;
    write(utf8);
}

// A blank cell, not a space: 0 is what the grid means by empty, and it still
// carries the pane's colours, which is what a status line is made of.
void Pane::fill_row()
{
    while (cx_ < w_)
        put(0);
}

void Pane::clear()
{
    for (u32 y = 0; y < h_; y++) {
        move(0, y);
        fill_row();
    }
    move(0, 0);
}

void Pane::place_cursor(u32 x, u32 y) const
{
    if (!w_ || !h_)
        return;
    screen_move(x_ + min(x, w_ - 1), y_ + min(y, h_ - 1));
}

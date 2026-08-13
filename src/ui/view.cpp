#include "view.h"

#include "kernel/traits.h"

void TextView::scroll(i32 delta, usize lines, u32 height)
{
    // The last line stays reachable, and a text shorter than the pane never
    // scrolls at all — a pager that can scroll into blankness looks broken.
    usize most = lines > height ? lines - height : 0;

    if (delta < 0) {
        usize up = usize(-delta);
        top_     = up > top_ ? 0 : top_ - up;
    } else {
        top_ += usize(delta);
    }
    if (top_ > most)
        top_ = most;
}

void TextView::to(usize row, usize lines, u32 height)
{
    usize most = lines > height ? lines - height : 0;
    top_       = row > most ? most : row;
}

void TextView::follow(usize row, usize col, u32 height, u32 width)
{
    if (!height || !width)
        return;
    if (row < top_)
        top_ = row;
    else if (row >= top_ + height)
        top_ = row - height + 1;

    if (col < left_)
        left_ = col;
    else if (col >= left_ + width)
        left_ = col - width + 1;
}

void TextView::paint(Pane &p, const TextBuf &b) const
{
    usize lines = b.lines();
    for (u32 y = 0; y < p.height(); y++) {
        p.move(0, y);
        usize row = top_ + y;
        if (row < lines) {
            Str s = b.line(row);
            p.write(s.substr(b.offset(row, left_)));
        }
        p.fill_row();
    }
}

// A window onto a TextBuf: which line is at the top of the pane, how many
// columns are scrolled off to the left, and the arithmetic that keeps a point
// visible. The pager and the editor differ in what they do with keys, not in
// how they scroll, so all of it is here and tested once.
#pragma once

#include "kernel/types.h"
#include "pane.h"
#include "textbuf.h"

struct TextView {
    usize top() const { return top_; }

    usize left() const { return left_; }

    // Moves the window by `delta` lines, clamped so the last line is never
    // scrolled past the top of the pane.
    void scroll(i32 delta, usize lines, u32 height);

    // Puts `row` at the top of the pane, under the same clamp.
    void to(usize row, usize lines, u32 height);

    // Moves the window as little as it takes to show (row, col).
    void follow(usize row, usize col, u32 height, u32 width);

    // Repaints every row of the pane, blanking those past the end of the text.
    void paint(Pane &p, const TextBuf &b) const;

private:
    usize top_  = 0;
    usize left_ = 0;
};

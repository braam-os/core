#include "harness.h"
#include "kernel/screen.h"
#include "ui/pane.h"
#include "ui/textbuf.h"
#include "ui/view.h"

namespace {

Str row(u32 y, char *out, usize cap)
{
    const Cell *cells = screen_cells();
    usize n           = 0;
    for (u32 x = 0; x < screen().cols && n < cap; x++) {
        char32_t ch = cells[y * screen().cols + x].ch;
        out[n++]    = ch && ch < 0x80 ? char(ch) : ' ';
    }
    while (n && out[n - 1] == ' ')
        n--;
    return Str(out, n);
}

} // namespace

void test_textbuf()
{
    test_begin("textbuf");

    TextBuf b;

    // An empty file is one empty line: there has to be somewhere to type.
    CHECK(b.load("").is_ok());
    CHECK_EQ(b.lines(), 1);
    CHECK(b.line(0).empty());
    CHECK(!b.modified());

    // A trailing newline ends the last line rather than adding an empty one.
    CHECK(b.load("one\ntwo\n").is_ok());
    CHECK_EQ(b.lines(), 2);
    CHECK(b.line(0) == "one");
    CHECK(b.line(1) == "two");
    CHECK(b.load("one\ntwo\n\n").is_ok());
    CHECK_EQ(b.lines(), 3);

    // What it read is what it writes back.
    CHECK(b.load("a\nbb\nccc").is_ok());
    String out;
    CHECK(b.serialize(out).is_ok());
    CHECK(out == "a\nbb\nccc\n");

    // Editing sets the modified flag, and clearing it is the save.
    CHECK(!b.modified());
    CHECK(b.insert(1, 1, "X").is_ok());
    CHECK(b.line(1) == "bXb");
    CHECK(b.modified());
    b.clear_modified();
    CHECK(!b.modified());

    CHECK_EQ(b.erase(1, 1), 1);
    CHECK(b.line(1) == "bb");
    CHECK_EQ(b.erase(1, 9), 0); // past the end erases nothing

    // Splitting and joining are the two halves of Enter and Backspace.
    CHECK(b.split(1, 1).is_ok());
    CHECK_EQ(b.lines(), 4);
    CHECK(b.line(1) == "b");
    CHECK(b.line(2) == "b");
    Result<usize> at = b.join(1);
    CHECK(at.is_ok());
    CHECK_EQ(at.value(), 1);
    CHECK(b.line(1) == "bb");
    CHECK(b.join(99).is_err());

    // Codepoints, not bytes: a two-byte character steps as one.
    CHECK(b.load("aé b").is_ok());
    CHECK_EQ(b.next(0, 0), 1);
    CHECK_EQ(b.next(0, 1), 3); // é is two bytes
    CHECK_EQ(b.prev(0, 3), 1);
    CHECK_EQ(b.column(0, 3), 2);
    CHECK_EQ(b.offset(0, 2), 3);
    CHECK_EQ(b.offset(0, 99), 5); // clamped to the end of the line
    CHECK_EQ(b.erase(0, 1), 2);   // the whole codepoint goes
    CHECK(b.line(0) == "a b");

    // A view over the buffer, painted into a pane over the test's own grid.
    screen_reset();
    CHECK(screen_resize(4, 3) != 0);
    CHECK(b.load("one\ntwo\nthree\nfour\nfive").is_ok());

    Grid g;
    g.cells = screen_cells();
    g.cols  = screen().cols;
    g.rows  = screen().rows;

    TextView v;
    Pane p = Pane::of(g);
    char buf[16];

    v.paint(p, b);
    CHECK(row(0, buf, sizeof(buf)) == "one");
    CHECK(row(2, buf, sizeof(buf)) == "thre"); // clipped, not wrapped

    // Scrolling stops with the last line on screen, so a pager cannot scroll
    // into blankness.
    v.scroll(2, b.lines(), 3);
    CHECK_EQ(v.top(), 2);
    v.scroll(9, b.lines(), 3);
    CHECK_EQ(v.top(), 2);
    v.scroll(-9, b.lines(), 3);
    CHECK_EQ(v.top(), 0);
    v.to(4, b.lines(), 3);
    CHECK_EQ(v.top(), 2);

    // A text shorter than the pane never scrolls at all.
    TextBuf small;
    CHECK(small.load("x").is_ok());
    TextView sv;
    sv.scroll(3, small.lines(), 3);
    CHECK_EQ(sv.top(), 0);

    // follow() moves the window as little as it takes to show a point.
    v.to(0, b.lines(), 3);
    v.follow(4, 0, 3, 4);
    CHECK_EQ(v.top(), 2);
    v.follow(0, 0, 3, 4);
    CHECK_EQ(v.top(), 0);
    v.follow(0, 6, 3, 4);
    CHECK_EQ(v.left(), 3);

    // A horizontal offset shows the tail of each line.
    v.paint(p, b);
    CHECK(row(2, buf, sizeof(buf)) == "ee");

    screen_reset();
}

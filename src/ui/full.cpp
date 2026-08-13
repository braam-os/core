#include "full.h"

#include "kernel/alloc.h"

FullScreen::FullScreen()
{
    const Screen &s = screen();
    if (!s.cols || !s.rows || !screen_cells())
        return;

    usize n = usize(s.cols) * s.rows * sizeof(Cell);
    saved_  = static_cast<Cell *>(heap_alloc(n));
    if (!saved_)
        return;

    __builtin_memcpy(saved_, screen_cells(), n);
    cols_      = s.cols;
    rows_      = s.rows;
    cursor_x_  = s.cursor_x;
    cursor_y_  = s.cursor_y;
    cursor_on_ = s.cursor_on != 0;

    screen_cursor(false);
    Pane::root().clear();
}

FullScreen::~FullScreen()
{
    if (!saved_)
        return;

    const Screen &s = screen();

    // A resize while the program ran leaves the snapshot describing a grid that
    // no longer exists. Blanking is the honest answer: the shell repaints its
    // prompt on the next line either way.
    if (s.cols == cols_ && s.rows == rows_ && screen_cells()) {
        __builtin_memcpy(screen_cells(), saved_, usize(cols_) * rows_ * sizeof(Cell));
        screen_touch(0, 0, cols_, rows_);
        screen_move(cursor_x_, cursor_y_);
    } else {
        screen_clear();
    }
    screen_cursor(cursor_on_);

    heap_free(saved_);
    saved_ = nullptr;
}

Pane FullScreen::body() const
{
    Pane r = Pane::root();
    return r.height() > 1 ? r.top(r.height() - 1) : r;
}

Pane FullScreen::status() const
{
    return Pane::root().bottom(1);
}

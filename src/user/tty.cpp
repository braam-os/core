#include "tty.h"

#include "io.h"
#include "kernel/alloc.h"
#include "kernel/screen.h"

namespace {

Result<usize> to_screen(void *, Str s)
{
    screen_write(s);
    return s.size();
}

// Pointers, so the globals stay trivially destructible (CLAUDE.md).
KeyRing *g_raw = nullptr;
Pipe *g_cooked = nullptr;

} // namespace

Stdio stdio_console()
{
    Stream s{ to_screen, nullptr, nullptr };
    return Stdio{ null_source(), s, s };
}

KeyInput::KeyInput()
{
    ring_ = static_cast<KeyRing *>(heap_alloc(sizeof(KeyRing)));
    if (!ring_)
        return;
    new (ring_) KeyRing();
    prev_ = g_raw;
    g_raw = ring_;
}

KeyInput::~KeyInput()
{
    if (!ring_)
        return;
    if (g_raw == ring_)
        g_raw = prev_;
    ring_->~KeyRing();
    heap_free(ring_);
}

InputClaim::InputClaim(Pipe *to) : prev_(g_cooked)
{
    g_cooked = to;
}

InputClaim::~InputClaim()
{
    g_cooked = prev_;
}

KeyRing *tty_raw()
{
    return g_raw;
}

Pipe *tty_cooked()
{
    return g_cooked;
}

// ------------------------------------------------------------ the alternate
// screen

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
    screen_clear();
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

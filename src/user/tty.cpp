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

// Pointers and words, so the globals stay trivially destructible (CLAUDE.md).
// Each route names its holder: the pid for the two a process claims, and the
// claim object itself for the cooked one, whose claimant is a builtin.
KeyRing *g_raw = nullptr;
u32 g_raw_pid  = 0;

FullScreen *g_alt = nullptr;
u32 g_alt_pid     = 0;

Pipe *g_cooked          = nullptr;
InputClaim *g_cooked_by = nullptr;

} // namespace

Stdio stdio_console()
{
    Stream s{ to_screen, nullptr, nullptr };
    return Stdio{ null_source(), s, s };
}

KeyInput::KeyInput(u32 pid)
{
    // Refused before anything is allocated: a claim that nested would leave the
    // pump pointing at a ring whose owner may die first.
    if (g_raw) {
        err_ = Error::Perm;
        return;
    }
    ring_ = static_cast<KeyRing *>(heap_alloc(sizeof(KeyRing)));
    if (!ring_)
        return;
    new (ring_) KeyRing();
    g_raw     = ring_;
    g_raw_pid = pid;
}

KeyInput::~KeyInput()
{
    if (!ring_)
        return;
    if (g_raw == ring_) {
        g_raw     = nullptr;
        g_raw_pid = 0;
    }
    ring_->~KeyRing();
    heap_free(ring_);
}

InputClaim::InputClaim(Pipe *to)
{
    if (g_cooked_by)
        return;
    g_cooked_by = this;
    g_cooked    = to;
    held_       = true;
}

InputClaim::~InputClaim()
{
    if (g_cooked_by != this)
        return;
    g_cooked_by = nullptr;
    g_cooked    = nullptr;
}

KeyRing *tty_raw()
{
    return g_raw;
}

Pipe *tty_cooked()
{
    return g_cooked;
}

u32 tty_keys_owner()
{
    return g_raw_pid;
}

u32 tty_screen_owner()
{
    return g_alt_pid;
}

// ------------------------------------------------------------ the alternate
// screen

FullScreen::FullScreen(u32 pid)
{
    // Before the snapshot, or a second claimant would save the blanked grid the
    // first is painting and give *that* back to the shell.
    if (g_alt) {
        err_ = Error::Perm;
        return;
    }

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

    g_alt     = this;
    g_alt_pid = pid;

    screen_cursor(false);
    screen_clear();
}

FullScreen::~FullScreen()
{
    if (!saved_)
        return;

    if (g_alt == this) {
        g_alt     = nullptr;
        g_alt_pid = 0;
    }

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

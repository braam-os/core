#include "tty.h"

#include "console.h"
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
// Each route names its holder by the pid that took it.
KeyRing *g_raw = nullptr;
u32 g_raw_pid  = 0;

FullScreen *g_alt = nullptr;
u32 g_alt_pid     = 0;

} // namespace

Stdio stdio_console()
{
    Stream s{ to_screen, nullptr, nullptr };
    return Stdio{ console_input(), s, s };
}

bool tty_is_console(const Stream &s)
{
    return s.fn == to_screen;
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

    // Type-ahead the claimant never read goes back to the console rather than
    // dying with the ring. The prompt is what makes this load bearing: a line
    // abandoned with ^C drops its claim with the rest of what was typed still
    // in it, and that has to reach the next prompt — which it did for free
    // while the editor received on keys() itself. The pump has drained the
    // channel by the time anything can release a claim, so this arrives ahead
    // of whatever the host queues next rather than behind it.
    for (Option<Key> k = ring_->try_recv(); k.has_value(); k = ring_->try_recv())
        keys().try_send(k.value());

    ring_->~KeyRing();
    heap_free(ring_);
}

KeyRing *tty_raw()
{
    return g_raw;
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

    // After the refusal and before the snapshot: a background job takes the
    // screen with no keystroke to have brought the view home first.
    screen_view_home();

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

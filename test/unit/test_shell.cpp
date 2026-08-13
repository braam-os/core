#include "harness.h"
#include "kernel/alloc.h"
#include "kernel/key.h"
#include "kernel/sched.h"
#include "kernel/screen.h"
#include "user/shell.h"

namespace {

u32 pid;

void press(u32 code, u32 mods = 0)
{
    keys().try_send(Key{ code, mods });
    sched_tick(0);
}

// Types a line and submits it, then ticks far enough for a sleep to expire.
void run(Str s)
{
    for (usize i = 0; i < s.size(); i++)
        press(u32(u8(s[i])));
    press(KEY_ENTER);
    sched_tick(100000);
}

Str row(u32 y)
{
    static char buf[SCREEN_MAX_COLS];
    const Cell *cells = screen_cells();
    usize n           = 0;
    for (u32 x = 0; x < screen().cols && n < sizeof(buf); x++) {
        char32_t ch = cells[y * screen().cols + x].ch;
        buf[n++]    = ch && ch < 0x80 ? char(ch) : ' ';
    }
    while (n && buf[n - 1] == ' ')
        n--;
    return Str(buf, n);
}

// Whether any row on screen reads exactly this.
bool has_row(Str want)
{
    for (u32 y = 0; y < screen().rows; y++)
        if (row(y) == want)
            return true;
    return false;
}

bool some_row_starts(Str want)
{
    for (u32 y = 0; y < screen().rows; y++)
        if (row(y).starts_with(want))
            return true;
    return false;
}

usize boot_cost; // heap taken by the shell itself, grid excluded

void boot(u32 cols, u32 rows)
{
    sched_reset();
    keys().clear();
    screen_reset();
    CHECK(screen_resize(cols, rows));

    usize before = heap_stats().bytes_in_use;
    pid          = sched_spawn(shell());
    CHECK(pid != 0);
    sched_tick(0);
    boot_cost = heap_stats().bytes_in_use - before;
}

} // namespace

void test_shell()
{
    test_begin("shell");

    // The shell's frame, the editor's, and the scheduler's job record all come
    // out of the heap. A coroutine frame past the allocator's 512-byte top
    // size class costs a whole 64 KiB span, so guard it here rather than
    // discover it in a profile.
    boot(40, 10);
    CHECK(boot_cost < 1024);

    // A prompt is drawn and the shell parks on the keyboard: nothing pending.
    CHECK(row(0) == "$");
    CHECK_EQ(sched_tick(0), -1);

    // echo prints its arguments, and success leaves a bare prompt.
    run("echo hello");
    CHECK(has_row("hello"));
    CHECK(has_row("$"));

    // A nonzero exit code is observable: it shows up in the next prompt.
    run("false");
    CHECK(has_row("[1] $"));
    run("nosuch");
    CHECK(some_row_starts("braam: nosuch: not found"));
    CHECK(has_row("[127] $"));
    run("true");
    CHECK(row(screen().cursor_y) == "$"); // back to a bare prompt

    // help lists the registered programs.
    boot(80, 16);
    run("help");
    CHECK(some_row_starts("  echo"));
    CHECK(some_row_starts("  help"));
    CHECK(some_row_starts("  version"));

    // Up recalls the previous command, and Enter runs it again.
    boot(40, 10);
    run("echo recalled");
    CHECK(has_row("recalled"));
    screen_clear();
    press(KEY_UP);
    press(KEY_ENTER);
    sched_tick(0);
    CHECK(has_row("recalled"));

    // ^C abandons the line and reports 130, without running anything.
    boot(40, 10);
    press('x');
    press('c', MOD_CTRL);
    sched_tick(0);
    CHECK(has_row("[130] $"));

    // A running program keeps the timer queue busy, and cancelling the shell
    // unwinds through it (Concept.md §8.1).
    boot(40, 10);
    for (char c : Str("sleep 5000"))
        press(u32(u8(c)));
    press(KEY_ENTER);
    CHECK(sched_tick(0) == 5000);
    CHECK(sched_alive(pid));
    sched_cancel(pid);
    sched_tick(0);
    CHECK(!sched_alive(pid));
    CHECK_EQ(sched_pending(), 0);

    sched_reset();
    screen_reset();
}

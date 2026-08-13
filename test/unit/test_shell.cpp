#include "harness.h"
#include "kernel/alloc.h"
#include "kernel/key.h"
#include "kernel/sched.h"
#include "kernel/screen.h"
#include "user/shell.h"

namespace {

u32 pid;

// Queues a key without ticking, which is how a burst of them really arrives:
// the host drains the ring once per tick, not once per key.
void queue(u32 code, u32 mods = 0)
{
    keys().try_send(Key{ code, mods });
}

void press(u32 code, u32 mods = 0)
{
    queue(code, mods);
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

usize rows_equal(Str want)
{
    usize n = 0;
    for (u32 y = 0; y < screen().rows; y++)
        if (row(y) == want)
            n++;
    return n;
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

    // M4's first criterion. `ls` lists the registry, which is what /bin will
    // hold, and grep filters it — two programs running at once over a pipe.
    boot(40, 16);
    run("ls | grep hel");
    CHECK(has_row("help"));
    CHECK(!has_row("echo"));
    CHECK(row(screen().cursor_y) == "$");

    // Quote removal reaches argv, and a pipeline's status is its last
    // command's: grep reports 1 when nothing matched.
    boot(40, 10);
    run("echo 'a b' | wc");
    CHECK(has_row("1 2 4"));
    run("ls | grep zzz");
    CHECK(has_row("[1] $"));

    // head stopping early is what closes the pipe under its producer: `ls`
    // gets Err(Closed) from its next write and stops, and the shell still
    // collects every stage.
    boot(40, 10);
    run("ls | head -n 2");
    CHECK(has_row("cat"));
    CHECK(has_row("clear"));
    CHECK(!has_row("echo"));
    CHECK_EQ(sched_pending(), 1); // only the shell is left

    // Three stages at once, and a redirection that has nowhere to go yet.
    boot(60, 16);
    run("ls | grep e | wc");
    CHECK(some_row_starts("9 9 "));
    run("echo hi > out.txt");
    CHECK(some_row_starts("braam: out.txt: no filesystem"));
    CHECK(has_row("[1] $"));
    run("echo 'a");
    CHECK(some_row_starts("braam: unterminated quote"));
    CHECK(has_row("[2] $"));

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

    // M4's second criterion: ^C reaches a running pipeline. The stages and
    // the pump are separate scheduler jobs, so this also proves none of them
    // is left behind.
    boot(40, 10);
    for (char c : Str("sleep 5000"))
        press(u32(u8(c)));
    press(KEY_ENTER);
    CHECK(sched_tick(0) == 5000);
    CHECK_EQ(sched_pending(), 3); // the shell, the stage and the pump
    press('c', MOD_CTRL);
    sched_tick(0);
    CHECK(has_row("^C"));
    CHECK(has_row("[130] $"));
    CHECK_EQ(sched_tick(0), -1); // the timer went with the stage
    CHECK_EQ(sched_pending(), 1);

    // The keyboard is handed back intact: the pump was its only receiver
    // while the job ran, and the shell must be able to edit again.
    run("echo after");
    CHECK(has_row("after"));

    // stdin is the pump's other job. A program with no pipe in front of it
    // reads what is typed — echoed once by the pump, printed again by cat —
    // and ^D is end of input rather than end of the pipeline.
    boot(40, 10);
    for (char c : Str("cat"))
        press(u32(u8(c)));
    press(KEY_ENTER);
    CHECK_EQ(sched_tick(0), -1); // cat is parked on its stdin, nothing pending
    CHECK_EQ(sched_pending(), 3);
    for (char c : Str("hi"))
        queue(u32(u8(c)));
    queue(KEY_ENTER);
    sched_tick(0);
    CHECK_EQ(rows_equal("hi"), 2); // the pump's echo, and cat's copy of it
    press('d', MOD_CTRL);
    sched_tick(0);
    CHECK(row(screen().cursor_y) == "$");
    CHECK_EQ(sched_pending(), 1);

    // Cancelling the shell mid-pipeline leaves nothing running. The children
    // are independent jobs, so this is the destructor in run_line's frame
    // doing the work a parent-child await would have done.
    boot(40, 10);
    for (char c : Str("sleep 5000"))
        press(u32(u8(c)));
    press(KEY_ENTER);
    CHECK(sched_tick(0) == 5000);
    CHECK_EQ(sched_pending(), 3);
    sched_cancel(pid);
    sched_tick(0);
    CHECK_EQ(sched_pending(), 0);
    CHECK_EQ(sched_tick(0), -1);

    // And the whole thing leaks nothing: booting, running a pipeline and
    // tearing down returns every byte.
    sched_reset();
    screen_reset();
    {
        usize in_use = heap_stats().bytes_in_use;
        boot(40, 10);
        run("ls | grep e | head -n 1");
        sched_reset();
        screen_reset(); // the grid is not the scheduler's to free
        CHECK_EQ(heap_stats().bytes_in_use, in_use);
    }

    sched_reset();
    screen_reset();
}

#include "fs/vfs.h"
#include "harness.h"
#include "user/io.h"
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

// Whether a file reads back as `want`, straight through the VFS. `cat` is a
// binary now and nothing in this module can run one, so a redirection is
// checked against the bytes rather than against a second program's idea of
// them. The comparison happens inside the coroutine because the buffer must
// not be a namespace-scope String — a non-trivial destructor there would want
// __cxa_atexit, which nothing provides (Concept.md §C.3).
bool matched;

Task<i32> compare(Str path, Str want, bool whole)
{
    matched = false;
    FileIo f;
    Task<Result<void>> o = file_open_read(path, f);
    if (!o)
        co_return 1;
    if (Result<void> r = co_await o; r.is_err())
        co_return 1;

    String text;
    for (;;) {
        u8 block[FS_BLOCK];
        Result<usize> r = vfs_read(f.fd, f.off, block, sizeof(block));
        if (r.is_err() || !r.value())
            break;
        f.off += r.value();
        if (!text.append(Str(reinterpret_cast<const char *>(block), r.value())))
            co_return 1;
    }
    matched = whole ? text.str() == want : text.str().starts_with(want);
    co_return 0;
}

bool file_is(Str path, Str want)
{
    sched_spawn(compare(path, want, true));
    sched_tick(0);
    return matched;
}

bool file_starts(Str path, Str want)
{
    sched_spawn(compare(path, want, false));
    sched_tick(0);
    return matched;
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
    //
    // The first boot builds the mount table, which is not the shell's cost and
    // is not the shell's to free; the guard is on a boot that finds one there
    // already, which is every boot after the first.
    vfs_reset();
    boot(40, 10);
    boot(40, 10);
    CHECK(boot_cost < 1024);

    // A prompt is drawn and the shell parks on the keyboard: nothing pending.
    CHECK(row(0) == "$");
    CHECK_EQ(sched_tick(0), -1);

    // A builtin runs, prints, and success leaves a bare prompt. Builtins are
    // all this module can run: every program is a binary now, and stepping one
    // means returning to the host, which run_tests() does once.
    run("jobs");
    CHECK(has_row("$"));

    // A nonzero exit code is observable: it shows up in the next prompt.
    run("kill %9");
    CHECK(has_row("kill: no such job"));
    CHECK(has_row("[1] $"));
    run("nosuch");
    CHECK(some_row_starts("braam: nosuch: not found"));
    CHECK(has_row("[127] $"));
    run("jobs");
    CHECK(row(screen().cursor_y) == "$"); // back to a bare prompt

    // help lists the builtins, in table order.
    boot(80, 40);
    run("help");
    CHECK(some_row_starts("  cd"));
    CHECK(some_row_starts("  help"));
    CHECK(some_row_starts("  kill"));

    // Up recalls the previous command, and Enter runs it again.
    boot(40, 10);
    run("kill %9");
    CHECK(has_row("kill: no such job"));
    screen_clear();
    press(KEY_UP);
    press(KEY_ENTER);
    sched_tick(0);
    CHECK(has_row("kill: no such job"));

    // ^C abandons the line and reports 130, without running anything.
    boot(40, 10);
    press('x');
    press('c', MOD_CTRL);
    sched_tick(0);
    CHECK(has_row("[130] $"));

    // A pipeline: two stages running at once over a bounded pipe, each an
    // independent scheduler job, and every one of them collected. What a
    // filter does with the bytes is asserted in run.mjs, where the stages can
    // be the real programs; what is asserted here is the runtime around them.
    boot(80, 40);
    run("jobs | help");
    CHECK(some_row_starts("  cd")); // the last stage's output reaches the screen
    CHECK(row(screen().cursor_y) == "$");
    CHECK_EQ(sched_pending(), 1); // only the shell is left

    // A pipeline's status is its last command's, and a stage that fails does
    // not take the pipeline's status with it.
    boot(40, 10);
    run("kill nonsense | jobs");
    CHECK(row(screen().cursor_y) == "$");
    run("jobs | kill nonsense");
    CHECK(has_row("[1] $"));

    // A parse error is the shell's own, and costs 2 before anything runs.
    boot(60, 16);
    run("help 'a");
    CHECK(some_row_starts("braam: unterminated quote"));
    CHECK(has_row("[2] $"));

    // A pipeline that stays running needs a program that parks, and every
    // program is a binary now: stepping one means returning to the host, which
    // run_tests() does once. So ^C reaching a running pipeline, the timer queue
    // it keeps busy, and cancelling the shell out from under one are all
    // asserted in run.mjs, against the real programs.
    //
    // ^C with nothing running is still this module's, and it is above.

    // M5. The shell starts in /home, and a redirection reaches a real file —
    // checked against the bytes, since reading it back would need a binary.
    // Surviving a reload needs a host and lives in run.mjs.
    boot(80, 40);
    CHECK(vfs_cwd() == "/home");
    run("help > notes");
    CHECK(file_starts("/home/notes", "  cd "));

    // `cd` is a builtin because the working directory is one global
    // (Concept.md §5.1), so the VFS is where its effect is visible.
    boot(40, 12);
    run("cd /tmp");
    CHECK(vfs_cwd() == "/tmp");
    run("cd ..");
    CHECK(vfs_cwd() == "/");
    run("cd");
    CHECK(vfs_cwd() == "/home"); // no argument means /home
    run("cd /nosuch");
    CHECK(some_row_starts("cd: /nosuch: not found"));
    CHECK(has_row("[1] $"));

    // A refused redirection stops the command before it can run: /proc is a
    // read-only filesystem, so nothing is written and nothing is printed.
    boot(40, 12);
    run("help > /proc/uptime");
    CHECK(some_row_starts("braam: /proc/uptime: "));
    CHECK(!some_row_starts("  cd"));
    CHECK(has_row("[1] $"));

    // A stage's stderr goes to its own file, the diagnostic is the stage's
    // rather than the shell's, and `>>` appends where `>` truncates.
    boot(40, 12);
    run("kill %9 2> /home/err");
    CHECK(file_is("/home/err", "kill: no such job\n"));
    run("kill %9 2>> /home/err");
    CHECK(file_is("/home/err", "kill: no such job\nkill: no such job\n"));
    run("kill %9 2> /home/err");
    CHECK(file_is("/home/err", "kill: no such job\n"));

    // And the whole thing leaks nothing: booting, running a pipeline and
    // tearing down returns every byte. The mount table goes too, since the
    // first boot in this case is what builds it.
    sched_reset();
    screen_reset();
    vfs_reset();
    {
        usize in_use = heap_stats().bytes_in_use;
        boot(40, 10);
        run("jobs | help");
        sched_reset();
        vfs_reset();
        screen_reset(); // the grid is not the scheduler's to free
        CHECK_EQ(heap_stats().bytes_in_use, in_use);
    }

    sched_reset();
    screen_reset();
    vfs_reset();
}

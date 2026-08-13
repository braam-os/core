#include "fs/vfs.h"
#include "harness.h"
#include "kernel/key.h"
#include "kernel/sched.h"
#include "kernel/screen.h"
#include "user/job.h"
#include "user/shell.h"

namespace {

void press(u32 code, u32 mods = 0)
{
    keys().try_send(Key{ code, mods });
    sched_tick(0);
}

// Types a line and submits it. The clock does not move, so a `sleep` started
// here is still running afterwards — which is the point of the cases below.
void submit(Str s)
{
    for (usize i = 0; i < s.size(); i++)
        press(u32(u8(s[i])));
    press(KEY_ENTER);
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

bool some_row_starts(Str want)
{
    for (u32 y = 0; y < screen().rows; y++)
        if (row(y).starts_with(want))
            return true;
    return false;
}

void boot(u32 cols, u32 rows)
{
    jobs_reset();
    sched_reset();
    keys().clear();
    screen_reset();
    CHECK(screen_resize(cols, rows));
    CHECK(sched_spawn(shell(), "shell") != 0);
    sched_tick(0);
}

} // namespace

void test_jobs()
{
    test_begin("jobs");

    // M7, second criterion. `&` starts the pipeline and comes straight back to
    // a prompt, and the job is in the table with its stages still running.
    boot(60, 12);
    CHECK_EQ(jobs_count(), 0);
    submit("sleep 5000 &");
    CHECK_EQ(jobs_count(), 1);

    JobInfo j;
    CHECK(jobs_at(0, j));
    CHECK_EQ(j.id, 1);
    CHECK(j.running);
    CHECK(j.cmd == "sleep 5000 &");
    CHECK(j.pid != 0);
    CHECK(sched_alive(j.pid));
    CHECK_EQ(jobs_current(), 1);
    CHECK(some_row_starts("[1] "));

    // The shell is at a prompt while it runs, and `jobs` lists it from there.
    submit("jobs");
    CHECK(some_row_starts("[1]+ running sleep 5000 &"));

    // A background job has no keyboard: it is not the pump's job, and its
    // stdin is at end of input from the start.
    submit("cat &");
    CHECK_EQ(jobs_count(), 2);
    sched_tick(0);
    CHECK(jobs_at(1, j));
    CHECK_EQ(j.id, 2);

    // Its finish is announced at the next prompt, and the entry is dropped.
    submit("");
    CHECK(some_row_starts("[2] done"));
    CHECK_EQ(jobs_count(), 1);

    // The timer fires; the sleeper finishes and is reported in its turn.
    sched_tick(6000);
    submit("");
    CHECK(some_row_starts("[1] done"));
    CHECK_EQ(jobs_count(), 0);

    // kill %n cancels every stage of a job. Cancellation is cooperative: the
    // sleeper unwinds at its await point, which is this same tick.
    boot(60, 12);
    submit("sleep 5000 &");
    CHECK(jobs_at(0, j));
    u32 sleeper = j.pid;
    CHECK(sched_alive(sleeper));
    submit("kill %1");
    CHECK(!sched_alive(sleeper));
    CHECK_EQ(jobs_count(), 0); // reported at the prompt and dropped
    submit("kill %9");
    CHECK(some_row_starts("kill: no such job"));

    // fg waits for a job in the foreground: the shell does not come back to a
    // prompt until the sleeper is done.
    boot(60, 12);
    submit("sleep 100 &");
    submit("fg");
    CHECK_EQ(jobs_count(), 1); // still there: fg has not finished waiting
    sched_tick(500);
    CHECK_EQ(jobs_count(), 0); // reaped by fg, not announced at the prompt
    CHECK(!some_row_starts("[1] done"));

    // ^C during fg reaches the job it adopted, through fg's own destructor:
    // the pump cancels the pipeline fg is a stage of, and fg passes it on.
    boot(60, 12);
    submit("sleep 5000 &");
    CHECK(jobs_at(0, j));
    sleeper = j.pid;
    submit("fg %1");
    CHECK(sched_alive(sleeper)); // fg is waiting; the job is still running
    press('c', MOD_CTRL);
    CHECK(!sched_alive(sleeper));

    submit("fg %9");
    CHECK(some_row_starts("fg: no such job"));

    jobs_reset();
    sched_reset();
    screen_reset();
    vfs_reset();
}

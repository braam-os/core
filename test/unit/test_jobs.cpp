#include "fs/vfs.h"
#include "harness.h"
#include "kernel/key.h"
#include "kernel/sched.h"
#include "kernel/screen.h"
#include "user/job.h"
#include "user/shell.h"

// The job table, as far as it can be driven without a program that keeps
// running. Filing a job means backgrounding a pipeline, and every program is a
// binary now: stepping one means returning to the host, and run_tests() does
// that once. So a job that stays running — `jobs` listing it, `fg` waiting for
// it, `^C` reaching what `fg` adopted, `kill %n` cancelling its stages — is
// asserted in run.mjs, against real programs. What is left here is the table's
// own behaviour: an empty table, the answers it gives for an id that is not in
// it, and a job that is filed and finishes within the tick that filed it.

namespace {

void press(u32 code, u32 mods = 0)
{
    keys().try_send(Key{ code, mods });
    sched_tick(0);
}

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

i32 waited;

Task<i32> wait_for(u32 id)
{
    waited                = -2;
    Task<Result<i32>> t   = jobs_wait(id);
    Result<i32> r         = t ? co_await t : Err(Error::NoMemory);
    waited                = r.is_ok() ? r.value() : -i32(r.error());
    co_return 0;
}

} // namespace

void test_jobs()
{
    test_begin("jobs");

    // An empty table answers, rather than being asked not to be empty.
    boot(60, 12);
    CHECK_EQ(jobs_count(), 0);
    CHECK_EQ(jobs_current(), 0);

    JobInfo j;
    CHECK(!jobs_at(0, j));
    CHECK(!jobs_find(1, j));
    CHECK(!jobs_kill(1));
    CHECK(jobs_input(1) == nullptr);

    // An unknown id is Err(Invalid) rather than a wait that never ends.
    sched_spawn(wait_for(9));
    sched_tick(0);
    CHECK_EQ(waited, -i32(Error::Invalid));

    // M7, second criterion, as far as a builtin reaches: `&` files the job,
    // comes straight back to a prompt, and announces it.
    boot(60, 12);
    submit("jobs &");
    CHECK_EQ(jobs_count(), 1);
    CHECK(jobs_at(0, j));
    CHECK_EQ(j.id, 1);
    CHECK(j.cmd == "jobs &");
    CHECK(j.pid != 0);
    CHECK_EQ(jobs_current(), 1);
    CHECK(some_row_starts("[1] "));

    // It finishes within the tick that ran it, so the next prompt announces it
    // and the entry is dropped — which is the half of the job table that needs
    // no program still running.
    sched_tick(0);
    submit("");
    CHECK(some_row_starts("[1] done"));
    CHECK_EQ(jobs_count(), 0);
    CHECK_EQ(jobs_current(), 0);

    // fg and kill both refuse an id that is not there, and say so.
    boot(60, 12);
    submit("fg %9");
    CHECK(some_row_starts("fg: no such job"));
    submit("kill %9");
    CHECK(some_row_starts("kill: no such job"));

    jobs_reset();
    sched_reset();
    screen_reset();
    vfs_reset();
}

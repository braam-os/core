#include "harness.h"
#include "kernel/key.h"
#include "kernel/sched.h"
#include "kernel/screen.h"
#include "kernel/string.h"
#include "user/console.h"
#include "user/tty.h"

// The pump, the foreground and the cooked input — the three things that make a
// shell running as a *process* possible, and the reason the pump is permanent
// rather than a job's. What cannot be asserted here is a program on the other
// end of Sys::Fg or Sys::Cursor: the in-wasm tests cannot run one, so that half
// is in run.mjs.

namespace {

u32 got[128];
usize got_n;
bool claimed;

// A claimant that reads raw keys, which is what a full-screen program and the
// prompt both are.
Task<i32> claimant()
{
    KeyInput claim(7);
    if (!claim.ok())
        co_return 1;
    claimed = true;

    for (;;) {
        Result<Key> r = co_await claim.next();
        if (r.is_err()) {
            if (r.error() == Error::Again)
                continue;
            co_return 0;
        }
        if (got_n < sizeof(got) / sizeof(got[0]))
            got[got_n++] = r.value().code;
    }
}

bool alive;

// Something for ^C to reach.
Task<i32> victim()
{
    alive = true;
    for (;;)
        if ((co_await Sleep(1000)).is_err())
            break;
    alive = false;
    co_return 130;
}

// A plain buffer rather than a String: a namespace-scope global has to be
// trivially destructible, since nothing provides __cxa_atexit.
char drained[64];
usize drained_n;
bool at_eof;

Str got_text()
{
    return Str(drained, drained_n);
}

// The foreground's stdin.
Task<i32> drain()
{
    Source in = console_input();
    for (;;) {
        Result<String> r = co_await in.read();
        if (r.is_err()) {
            if (r.error() == Error::Again)
                continue;
            at_eof = r.error() == Error::Closed;
            co_return 0;
        }
        for (usize i = 0; i < r.value().size() && drained_n < sizeof(drained); i++)
            drained[drained_n++] = r.value()[i];
    }
}

void press(u32 code, u32 mods = 0)
{
    keys().try_send(Key{ code, mods });
}

void type(Str s)
{
    for (usize i = 0; i < s.size(); i++)
        press(u32(u8(s[i])));
}

f64 clock_ms;

// Ticks until nothing is due, which is what both the host and the test driver
// do: the pump yields through the timer queue when a ring is full.
void settle()
{
    for (usize i = 0; i < 8; i++)
        if (sched_tick(clock_ms += 1) != 0)
            return;
}

} // namespace

void test_console()
{
    test_begin("console");

    sched_reset();
    keys().clear();
    screen_reset();
    CHECK(screen_resize(40, 10));
    console_fg_clear();
    drained_n = 0;

    CHECK(sched_spawn(console_pump(), "tty") != 0);
    CHECK_EQ(console_fg_count(), 0u);

    // With nobody in front, ^C is an ordinary key and reaches the claimant.
    // That is the whole reason a shell can be a process: the alternative is
    // being cancelled by the ^C that was meant to abandon a line.
    got_n    = 0;
    claimed  = false;
    u32 cpid = sched_spawn(claimant());
    CHECK(cpid != 0);
    settle();
    CHECK(claimed);

    u32 before = console_interrupts();
    press('c', MOD_CTRL);
    settle();
    CHECK_EQ(got_n, 1u);
    CHECK_EQ(got[0], u32('c'));
    CHECK_EQ(console_interrupts(), before);

    // A burst longer than the ring still arrives whole. The pump never
    // suspends while the channel has something in it, so without a yield the
    // tail of a typed line was dropped before the claimant was ever scheduled.
    got_n     = 0;
    usize big = KEY_RING + 8;
    for (usize i = 0; i < big; i++)
        press(u32('a' + (i % 26)));
    settle();
    CHECK_EQ(got_n, big);
    CHECK_EQ(got[big - 1], u32('a' + ((big - 1) % 26)));

    sched_cancel(cpid);
    settle();
    CHECK(tty_raw() == nullptr);

    // Nothing claimed: keys are cooked, a line at a time, and reach whatever is
    // reading the console's own stdin.
    u32 dpid = sched_spawn(drain());
    CHECK(dpid != 0);
    settle();
    type("hi");
    press(KEY_ENTER);
    settle();
    CHECK(got_text() == "hi\n");

    // ^D is end of input for the foreground, not for the console.
    type("bye");
    press('d', MOD_CTRL);
    settle();
    CHECK(got_text() == "hi\nbye");
    CHECK(at_eof);

    // Arming a new foreground reopens it, and drops type-ahead meant for what
    // has gone.
    at_eof    = false;
    drained_n = 0;
    press('x');
    press(KEY_ENTER);
    settle();

    u32 vpid = sched_spawn(victim());
    CHECK(vpid != 0);
    settle();
    CHECK(alive);
    CHECK(console_fg_set(&vpid, 1));
    CHECK_EQ(console_fg_count(), 1u);
    CHECK(console_fg_has(vpid));
    CHECK(!console_fg_has(vpid + 1000));

    dpid = sched_spawn(drain());
    CHECK(dpid != 0);
    settle();
    type("now");
    press(KEY_ENTER);
    settle();
    CHECK(got_text() == "now\n"); // not "x\nnow\n"
    CHECK(!at_eof);

    // ^C reaches what is in front, is counted rather than delivered, and the
    // count is what the shell samples around a pipeline.
    before = console_interrupts();
    press('c', MOD_CTRL);
    settle();
    CHECK_EQ(console_interrupts(), before + 1);
    CHECK(!alive);

    // More pids than there is room for is refused rather than truncated.
    u32 many[CONSOLE_FG_MAX + 1] = {};
    CHECK(!console_fg_set(many, CONSOLE_FG_MAX + 1));
    CHECK(console_fg_set(many, CONSOLE_FG_MAX));
    console_fg_clear();
    CHECK_EQ(console_fg_count(), 0u);

    sched_reset();
}

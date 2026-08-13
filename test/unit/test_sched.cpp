#include "harness.h"

#include "kernel/alloc.h"
#include "kernel/sched.h"
#include "kernel/str.h"

namespace {

// Tasks append to this instead of logging, so order is checkable.
char trace[32];
usize trace_n;

void mark(char c) {
    if (trace_n < sizeof(trace))
        trace[trace_n++] = c;
}

Str traced() {
    return Str(trace, trace_n);
}

Task<i32> sleeper(char first, u32 a, char second, u32 b) {
    if ((co_await sleep_ms(a)).is_err())
        co_return 1;
    mark(first);
    if ((co_await sleep_ms(b)).is_err())
        co_return 1;
    mark(second);
    co_return 0;
}

int live_guards;

struct Guard {
    Guard() { live_guards++; }

    ~Guard() { live_guards--; }
};

// Holds a destructor across the sleep, which is what cancellation must run.
Task<i32> guarded_sleeper(u32 ms) {
    Guard g;
    if ((co_await sleep_ms(ms)).is_err()) {
        mark('x');
        co_return 1;
    }
    mark('t');
    co_return 0;
}

u32 wake_token;

Task<i32> waiter() {
    Wake ev;
    wake_token = ev.token();
    Result<Payload> r = co_await ev;
    if (r.is_err())
        co_return 1;
    mark(char('0' + r.value().len));
    co_return 0;
}

} // namespace

void test_sched() {
    test_begin("sched");

    // Two coroutines interleave their sleeps: a at 10 and 30, b at 15 and 25.
    sched_reset();
    trace_n = 0;
    u32 a = sched_spawn(sleeper('a', 10, 'A', 20));
    u32 b = sched_spawn(sleeper('b', 15, 'B', 10));
    CHECK(a != 0);
    CHECK(b != 0);
    CHECK_EQ(sched_tick(0), 10);
    CHECK(traced() == "");
    CHECK_EQ(sched_tick(10), 5);
    CHECK(traced() == "a");
    CHECK_EQ(sched_tick(15), 10);
    CHECK(traced() == "ab");
    CHECK_EQ(sched_tick(25), 5);
    CHECK(traced() == "abB");
    CHECK(sched_alive(a));
    CHECK(!sched_alive(b));
    CHECK_EQ(sched_tick(30), -1);
    CHECK(traced() == "abBA");
    CHECK(!sched_alive(a));

    // A tick before the deadline resumes nobody and repeats the delay.
    sched_reset();
    trace_n = 0;
    sched_spawn(sleeper('c', 100, 'C', 100));
    CHECK_EQ(sched_tick(0), 100);
    CHECK_EQ(sched_tick(40), 60);
    CHECK(traced() == "");

    // Cancelling a sleeping task unwinds it and runs its destructors.
    sched_reset();
    trace_n = 0;
    live_guards = 0;
    usize in_use = heap_stats().bytes_in_use;
    u32 pid = sched_spawn(guarded_sleeper(1000));
    CHECK_EQ(sched_tick(0), 1000);
    CHECK_EQ(live_guards, 1);
    sched_cancel(pid);
    CHECK_EQ(sched_tick(1), -1); // the timer left with the task
    CHECK_EQ(live_guards, 0);
    CHECK(traced() == "x");
    CHECK(!sched_alive(pid));
    sched_reset();
    CHECK_EQ(heap_stats().bytes_in_use, in_use);

    // A wake token carries its payload to the task waiting on it.
    sched_reset();
    trace_n = 0;
    wake_token = 0;
    u32 w = sched_spawn(waiter());
    CHECK_EQ(sched_tick(0), -1); // suspended on a token, with no timer
    CHECK(wake_token != 0);
    sched_wake(wake_token + 1000, 0, 1); // an unknown token is ignored
    CHECK_EQ(sched_tick(1), -1);
    CHECK(traced() == "");
    CHECK(sched_alive(w));
    sched_wake(wake_token, 0, 7);
    CHECK_EQ(sched_tick(2), -1);
    CHECK(traced() == "7");
    CHECK(!sched_alive(w));

    // Cancelling a token wait deregisters it, so a late wake finds nobody.
    sched_reset();
    trace_n = 0;
    wake_token = 0;
    u32 c = sched_spawn(waiter());
    sched_tick(0);
    sched_cancel(c);
    CHECK_EQ(sched_tick(1), -1);
    CHECK(!sched_alive(c));
    sched_wake(wake_token, 0, 3);
    CHECK_EQ(sched_tick(2), -1);
    CHECK(traced() == "");

    sched_reset();
}

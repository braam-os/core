#include "harness.h"

#include "kernel/alloc.h"
#include "kernel/channel.h"
#include "kernel/sched.h"
#include "kernel/str.h"

namespace {

using Chan = Channel<u32, 4>;

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

// The channel outlives every task that references it: it is a local of
// test_channel(), and sched_reset() destroys the frames first.
Task<i32> drain(Chan &c, u32 n) {
    for (u32 i = 0; i < n; i++) {
        Result<u32> r = co_await c.recv();
        if (r.is_err()) {
            mark('x');
            co_return 1;
        }
        mark(char('0' + r.value()));
    }
    co_return 0;
}

int live_guards;

struct Guard {
    Guard() { live_guards++; }

    ~Guard() { live_guards--; }
};

// Holds a destructor across the suspension, which cancellation must run.
Task<i32> guarded(Chan &c) {
    Guard g;
    Result<u32> r = co_await c.recv();
    if (r.is_err()) {
        mark('x');
        co_return 1;
    }
    mark(char('0' + r.value()));
    co_return 0;
}

} // namespace

void test_channel() {
    test_begin("channel");

    // A value already queued is taken without suspending.
    sched_reset();
    trace_n = 0;
    {
        Chan c;
        CHECK(c.empty());
        CHECK(c.try_send(1));
        CHECK_EQ(c.size(), 1);
        CHECK(sched_spawn(drain(c, 1)) != 0);
        CHECK_EQ(sched_tick(0), -1);
        CHECK(traced() == "1");
        CHECK(c.empty());
    }

    // An empty channel suspends the receiver, with no timer behind it.
    sched_reset();
    trace_n = 0;
    {
        Chan c;
        u32 pid = sched_spawn(drain(c, 1));
        CHECK_EQ(sched_tick(0), -1);
        CHECK(traced() == "");
        CHECK(sched_alive(pid));
        CHECK(c.try_send(7));
        CHECK_EQ(sched_tick(1), -1);
        CHECK(traced() == "7");
        CHECK(!sched_alive(pid));
    }

    // The ring is FIFO and wraps: five values through a capacity of four.
    sched_reset();
    trace_n = 0;
    {
        Chan c;
        CHECK(c.try_send(1));
        CHECK(c.try_send(2));
        CHECK(c.try_send(3));
        sched_spawn(drain(c, 5));
        CHECK_EQ(sched_tick(0), -1);
        CHECK(traced() == "123"); // then it parks, waiting for more
        CHECK(c.try_send(4));
        CHECK(c.try_send(5));
        CHECK_EQ(sched_tick(1), -1);
        CHECK(traced() == "12345");
    }

    // A full ring drops the newest value and keeps what it already holds.
    sched_reset();
    trace_n = 0;
    {
        Chan c;
        for (u32 i = 1; i <= 4; i++)
            CHECK(c.try_send(i));
        CHECK(c.full());
        CHECK(!c.try_send(9));
        CHECK_EQ(c.size(), 4);
        sched_spawn(drain(c, 4));
        CHECK_EQ(sched_tick(0), -1);
        CHECK(traced() == "1234");
    }

    // Cancelling a parked receiver unwinds it and runs its destructors; the
    // awaiter deregisters, so a later send finds nobody and keeps its value.
    sched_reset();
    trace_n = 0;
    live_guards = 0;
    usize in_use = heap_stats().bytes_in_use;
    {
        Chan c;
        u32 pid = sched_spawn(guarded(c));
        CHECK_EQ(sched_tick(0), -1);
        CHECK_EQ(live_guards, 1);
        sched_cancel(pid);
        CHECK_EQ(sched_tick(1), -1);
        CHECK_EQ(live_guards, 0);
        CHECK(traced() == "x");
        CHECK(!sched_alive(pid));

        CHECK(c.try_send(3));
        CHECK_EQ(sched_tick(2), -1);
        CHECK(traced() == "x");
        CHECK_EQ(c.size(), 1);
    }
    sched_reset();
    CHECK_EQ(heap_stats().bytes_in_use, in_use);

    // Destroying a parked receiver with the scheduler leaves the channel
    // usable, which is the awaiter-destructor rule doing its job.
    trace_n = 0;
    {
        Chan c;
        sched_spawn(drain(c, 1));
        CHECK_EQ(sched_tick(0), -1);
        sched_reset();
        CHECK(c.try_send(5));
        CHECK_EQ(c.size(), 1);
        sched_spawn(drain(c, 1));
        CHECK_EQ(sched_tick(0), -1);
        CHECK(traced() == "5");
    }

    sched_reset();
}

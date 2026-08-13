#include "harness.h"
#include "kernel/alloc.h"
#include "kernel/hostcall.h"
#include "kernel/jsref.h"
#include "kernel/sched.h"
#include "kernel/traits.h"
#include "svc/net.h"
#include "svc/svc.h"

// The services behind these cases are test/fakesvc.mjs. The record machinery
// is the storage one — test_hostfs covers the orphan path — so what is new
// here is the slot a reply deposits into, and its two ends: it is handed over
// on success and released when the request never happens.

namespace {

// A namespace-scope global must be trivially destructible, so what was heard
// is a flag rather than the String it came in.
WallClock clock_read;
Error failure;
bool heard_ping;
bool answered;

Task<i32> ask_clock()
{
    Task<Result<WallClock>> t = svc_clock();
    if (!t)
        co_return 1;
    Result<WallClock> r = co_await t;
    answered            = true;
    if (r.is_err()) {
        failure = r.error();
        co_return 1;
    }
    clock_read = r.value();
    co_return 0;
}

// Open, send, receive: the loopback socket in the fake delivers to itself when
// it is the only one, so one task exercises the whole path.
Task<i32> ask_socket()
{
    Result<WebSocket> open = Err(Error::NoMemory);
    if (Task<Result<WebSocket>> t = ws_open("ws://loop"))
        open = co_await t;
    if (open.is_err()) {
        failure = open.error();
        co_return 1;
    }

    if (Task<Result<void>> t = ws_send(open.value(), "ping")) {
        Result<void> sent = co_await t;
        if (sent.is_err()) {
            failure = sent.error();
            co_return 1;
        }
    }

    if (Task<Result<String>> t = ws_recv(open.value())) {
        Result<String> got = co_await t;
        if (got.is_err()) {
            failure = got.error();
            co_return 1;
        }
        heard_ping = got.value().str() == "ping";
    }
    answered = true;
    co_return 0;
}

} // namespace

void test_svc()
{
    test_begin("svc");

    usize in_use = heap_stats().bytes_in_use;
    usize live   = jsref_live();

    // The wall clock, which is what the kernel's own monotonic clock cannot be.
    sched_reset();
    answered = false;
    CHECK(sched_spawn(ask_clock()) != 0);
    CHECK_EQ(sched_tick(0), -1);
    CHECK(answered);
    CHECK(clock_read.epoch_ms > 0);
    CHECK_EQ(host_orphans(), 0);

    // A socket is a slot the host deposits into and the kernel then owns; the
    // count comes back to where it started once the handle goes away.
    sched_reset();
    answered   = false;
    heard_ping = false;
    CHECK(sched_spawn(ask_socket()) != 0);
    CHECK_EQ(sched_tick(0), -1);
    CHECK(answered);
    CHECK(heard_ping);
    CHECK_EQ(jsref_live(), live);
    CHECK_EQ(host_orphans(), 0);

    // A request cancelled before it is issued never reaches the host, so the
    // slot it claimed for the reply has to come back with the record. It is
    // the one way the externref table can leak without anyone noticing.
    sched_reset();
    answered = false;
    failure  = Error::Invalid;
    u32 pid  = sched_spawn(ask_socket());
    CHECK(pid != 0);
    sched_cancel(pid);
    CHECK_EQ(sched_tick(0), -1);
    CHECK(!answered);
    CHECK(failure == Error::Cancelled);
    CHECK_EQ(jsref_live(), live);
    CHECK_EQ(host_orphans(), 0);

    sched_reset();
    CHECK_EQ(heap_stats().bytes_in_use, in_use);
}

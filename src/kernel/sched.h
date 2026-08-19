// The scheduler: a ready queue, a timer queue and a wake-token table
// (Concept.md §3.3). tick() is the only thing that ever resumes a coroutine.
#pragma once

#include "coroutine.h"
#include "result.h"
#include "str.h"
#include "task.h"
#include "types.h"

struct Waiter;

// One per task tree. Killing means signalling it; every await point checks it
// and unwinds by returning (Concept.md §3.6, §8.1).
struct CancelState {
    bool cancelled  = false;
    Waiter *waiting = nullptr; // the awaiter this tree is suspended on
};

struct CancelToken {
    CancelState *s = nullptr;

    bool cancelled() const { return s && s->cancelled; }
};

// A suspension record. It lives inside the suspended coroutine's frame, so
// registering costs no allocation and wake() has somewhere to put a payload.
struct Waiter {
    std::coroutine_handle<> h;
    CancelState *cancel = nullptr;
    u32 token           = 0;
    u32 payload_ptr     = 0;
    u32 payload_len     = 0;
    bool timed          = false; // in the timer queue
    bool listed         = false; // in the wake table, under token
    bool parked         = false; // the token is a channel's, not a host call's
    bool cancelled      = false; // woken by a cancellation rather than an event
    bool failed         = false; // could not be registered
};

// What wake(token, ptr, len) delivers.
struct Payload {
    u32 ptr = 0;
    u32 len = 0;
};

// One line of /proc: what the scheduler knows about a task.
struct ProcInfo {
    // Where a suspended task is registered, which is as much as the scheduler
    // can say about what it is waiting for: the timer queue, the wake table (so
    // a host call), or neither — parked on a channel, a pipe or the keyboard.
    enum class Wait : u8 { None, Timer, Host, Park };

    u32 pid;
    Str name;
    bool waiting;   // suspended on an awaitable
    bool cancelled; // signalled, but not yet unwound
    Wait wait;
    f64 started; // sched_now() when it was spawned
};

// What the scheduler has done since it was built, and what it holds now, which
// is what /proc/stat publishes. A reset zeroes the counters with the scheduler.
struct SchedStats {
    u64 ticks;   // sched_tick calls: turns of the event loop
    u64 resumes; // coroutine resumptions, which only tick() performs
    u64 wakes;   // a woken token, split the way /proc splits a suspended task:
    u64 unparks; // an answer from outside, or a channel's own traffic
    u64 misses;  // woken with nothing waiting: a late or cancelled event
    u64 timers;  // timer expiries
    u64 spawns;  // tasks created; a syscall server is one, so not a fork rate

    // Gauges. The four below partition `tasks`.
    usize tasks;
    usize ready; // suspended on nothing
    usize on_timer;
    usize on_host;
    usize on_park;
};

SchedStats sched_stats();

// `name` is stored as a view, so it must outlive the task: a literal, or a
// stage's argv[0] out of the job's word store — never a local. Returns 0 on
// failure, an exhausted pid space among them.
u32 sched_spawn(Task<i32> t, Str name = {});
void sched_cancel(u32 pid);
bool sched_alive(u32 pid);

// A snapshot of the live tasks, up to `cap`. Empty while the scheduler is being
// torn down, when jobs[] holds freed pointers.
usize sched_procs(ProcInfo *out, usize cap);
i32 sched_tick(f64 now_ms);

// False when nothing was waiting on the token: a late or cancelled event, which
// the storage layer needs to hear about so it can free an abandoned request.
bool sched_wake(u32 token, u32 ptr, u32 len);
f64 sched_now();
usize sched_pending();
void sched_reset();

u32 sched_token();
bool sched_wait_timer(Waiter *w, u32 ms);
bool sched_wait_token(Waiter *w);
void sched_unwait(Waiter *w);

// Suspends until the deadline passes, or until the task is cancelled.
struct Sleep {
    explicit Sleep(u32 ms) : ms_(ms) {}

    Sleep(const Sleep &)            = delete;
    Sleep &operator=(const Sleep &) = delete;

    ~Sleep() { sched_unwait(&w_); }

    bool await_ready() const noexcept { return false; }

    template <class P>
    bool await_suspend(std::coroutine_handle<P> h)
    {
        w_.h      = h;
        w_.cancel = h.promise().cancel;
        if (w_.cancel && w_.cancel->cancelled) {
            w_.cancelled = true;
            return false;
        }
        if (!sched_wait_timer(&w_, ms_)) {
            w_.failed = true;
            return false;
        }
        return true;
    }

    Result<void> await_resume() const
    {
        if (w_.cancelled || (w_.cancel && w_.cancel->cancelled))
            return Err(Error::Cancelled);
        if (w_.failed)
            return Err(Error::NoMemory);
        return {};
    }

private:
    u32 ms_;
    Waiter w_;
};

// Suspends until wake(token) arrives. The token is readable before the
// co_await, which is where an async import announces it to the host.
struct Wake {
    Wake() { w_.token = sched_token(); }

    Wake(const Wake &)            = delete;
    Wake &operator=(const Wake &) = delete;

    ~Wake() { sched_unwait(&w_); }

    u32 token() const { return w_.token; }

    bool await_ready() const noexcept { return false; }

    template <class P>
    bool await_suspend(std::coroutine_handle<P> h)
    {
        w_.h      = h;
        w_.cancel = h.promise().cancel;
        if (w_.cancel && w_.cancel->cancelled) {
            w_.cancelled = true;
            return false;
        }
        if (!sched_wait_token(&w_)) {
            w_.failed = true;
            return false;
        }
        return true;
    }

    Result<Payload> await_resume() const
    {
        if (w_.cancelled || (w_.cancel && w_.cancel->cancelled))
            return Err(Error::Cancelled);
        if (w_.failed)
            return Err(Error::NoMemory);
        return Payload{ w_.payload_ptr, w_.payload_len };
    }

private:
    Waiter w_;
};

Task<Result<void>> sleep_ms(u32 ms);

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
    u32 pid;
    Str name;
    bool waiting;   // suspended on an awaitable
    bool cancelled; // signalled, but not yet unwound
};

// `name` is stored as a view, so it must outlive the task: a literal, or a
// Program's own name — never a local.
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

// argv, stdio and the pipes behind them — what a command is entered with,
// whichever side of the process boundary it runs on (Concept.md §3.6).
#pragma once

#include "kernel/coroutine.h"
#include "kernel/result.h"
#include "kernel/sched.h"
#include "kernel/span.h"
#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/task.h"
#include "kernel/types.h"

// argv. The words are views into the shell's line buffer, which outlives the
// program; nothing here owns anything.
struct Args {
    Span<const Str> v;

    usize size() const { return v.size(); }

    Str operator[](usize i) const { return v[i]; }

    Str name() const { return v.empty() ? Str() : v[0]; }

    Args tail() const { return Args{ v.subspan(1) }; }
};

// A byte sink: the console, or a pipe. A function pointer rather than a
// vtable, because the implementations are few and known.
//
// A sink that has no room reports Err(Again) and is expected to arm `park`
// with a wake token; the awaitable below suspends on it and tries once more.
// A sink whose reader is gone reports Err(Closed), which is how a program
// upstream of `head` learns to stop.
struct Stream {
    using WriteFn = Result<usize> (*)(void *ctx, Str s);
    using ParkFn  = void (*)(void *ctx, u32 token, bool on);

    WriteFn fn  = nullptr;
    ParkFn park = nullptr;
    void *ctx   = nullptr;

    // The work happens in await_suspend rather than await_ready because only
    // await_suspend can reach the promise, and therefore the cancel state
    // (Concept.md §8.1). A sink may not retain `data` past this awaitable.
    // The sink is held field by field rather than as a Stream, because the
    // enclosing class is still incomplete here.
    struct Write {
        Write(WriteFn fn, ParkFn park, void *ctx, Str data)
            : fn_(fn), park_(park), ctx_(ctx), data_(data)
        {
        }

        Write(const Write &)            = delete;
        Write &operator=(const Write &) = delete;

        // Deregistering here is what makes destroying a frame parked in a
        // write safe, rather than a dangling Waiter in the wake table.
        ~Write()
        {
            if (w_.token && park_)
                park_(ctx_, w_.token, false);
            sched_unwait(&w_);
        }

        bool await_ready() const noexcept { return false; }

        template <class P>
        bool await_suspend(std::coroutine_handle<P> h)
        {
            w_.cancel = h.promise().cancel;
            if (w_.cancel && w_.cancel->cancelled) {
                r_ = Err(Error::Cancelled);
                return false;
            }
            if (!fn_) {
                r_ = Err(Error::Invalid);
                return false;
            }
            r_ = fn_(ctx_, data_);
            if (r_.is_ok() || r_.error() != Error::Again || !park_)
                return false;

            w_.h     = h;
            w_.token = sched_token();
            if (!sched_wait_token(&w_)) {
                w_.token = 0;
                r_       = Err(Error::NoMemory);
                return false;
            }
            park_(ctx_, w_.token, true);
            return true;
        }

        // One retry: with a single writer per sink, being woken by the reader
        // means there is room. A second Again is a stray wake, not a full ring.
        Result<usize> await_resume()
        {
            if (r_.is_ok() || r_.error() != Error::Again)
                return r_;
            if (w_.cancelled || (w_.cancel && w_.cancel->cancelled))
                return Err(Error::Cancelled);
            return fn_(ctx_, data_);
        }

    private:
        WriteFn fn_;
        ParkFn park_;
        void *ctx_;
        Str data_;
        Result<usize> r_ = Err(Error::Invalid);
        Waiter w_;
    };

    Write write(Str s) const { return Write{ fn, park, ctx, s }; }
};

// A byte source: a pipe, or a stream that is already at EOF. The mirror of
// Stream, with the same park protocol. Err(Closed) is end of input.
struct Source {
    using ReadFn = Result<String> (*)(void *ctx);
    using ParkFn = void (*)(void *ctx, u32 token, bool on);

    ReadFn fn   = nullptr;
    ParkFn park = nullptr;
    void *ctx   = nullptr;

    struct Read {
        Read(ReadFn fn, ParkFn park, void *ctx) : fn_(fn), park_(park), ctx_(ctx) {}

        Read(const Read &)            = delete;
        Read &operator=(const Read &) = delete;

        ~Read()
        {
            if (w_.token && park_)
                park_(ctx_, w_.token, false);
            sched_unwait(&w_);
        }

        bool await_ready() const noexcept { return false; }

        template <class P>
        bool await_suspend(std::coroutine_handle<P> h)
        {
            w_.cancel = h.promise().cancel;
            if (w_.cancel && w_.cancel->cancelled) {
                r_ = Err(Error::Cancelled);
                return false;
            }
            if (!fn_) {
                r_ = Err(Error::Closed);
                return false;
            }
            r_ = fn_(ctx_);
            if (r_.is_ok() || r_.error() != Error::Again || !park_)
                return false;

            w_.h     = h;
            w_.token = sched_token();
            if (!sched_wait_token(&w_)) {
                w_.token = 0;
                r_       = Err(Error::NoMemory);
                return false;
            }
            park_(ctx_, w_.token, true);
            return true;
        }

        Result<String> await_resume()
        {
            if (r_.is_ok() || r_.error() != Error::Again)
                return move(r_);
            if (w_.cancelled || (w_.cancel && w_.cancel->cancelled))
                return Err(Error::Cancelled);
            return fn_(ctx_);
        }

    private:
        ReadFn fn_;
        ParkFn park_;
        void *ctx_;
        Result<String> r_ = Err(Error::Invalid);
        Waiter w_;
    };

    Read read() const { return Read{ fn, park, ctx }; }
};

// Concept.md §3.6's stdio. `in` is empty rather than absent for a program the
// shell gave no input: reading it reports EOF immediately.
struct Stdio {
    Source in;
    Stream out, err;
};

// What the job runtime enters, whether it is a shell builtin or the proxy task
// standing in for a process. There is no registry behind it any more: a program
// is a file in /bin, and the only things with this shape inside the kernel are
// the six builtins (builtin.h).
using ProgramFn = Task<i32> (*)(Args, Stdio);

// The program registry (Concept.md §3.6): a list of Task<i32>(Args, Stdio)
// functions, populated at static-init time. One self-registering file per
// program in src/prog/, so adding a command edits nothing else.
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

using ProgramFn = Task<i32> (*)(Args, Stdio);

// A descriptor, linked into the registry at static-init time. Trivially
// destructible, so it needs no __cxa_atexit.
struct Program {
    Str name;
    Str usage; // one line, what `help` prints
    ProgramFn run;
    Program *next; // program_register links it in, sorted by name
};

void program_register(Program &p);

const Program *program_find(Str name);

// Iteration, in name order: for (const Program *p = program_first(); p; p = p->next)
const Program *program_first();

struct ProgramRegistrar {
    explicit ProgramRegistrar(Program &p) { program_register(p); }
};

// Defines a program and registers it. The body sees `args` and `io`.
#define BRAAM_PROGRAM(fn, prog_name, prog_usage)                                                \
    static Task<i32> fn(Args, Stdio);                                                           \
    static Program fn##_desc{ prog_name, prog_usage, fn, nullptr };                             \
    [[maybe_unused]] __attribute__((used)) static const ProgramRegistrar fn##_reg{ fn##_desc }; \
    static Task<i32> fn([[maybe_unused]] Args args, [[maybe_unused]] Stdio io)

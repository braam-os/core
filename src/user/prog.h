// The program registry (Concept.md §3.6): a list of Task<i32>(Args, Stdio)
// functions, populated at static-init time. One self-registering file per
// program in src/prog/, so adding a command edits nothing else.
#pragma once

#include "kernel/coroutine.h"
#include "kernel/result.h"
#include "kernel/sched.h"
#include "kernel/span.h"
#include "kernel/str.h"
#include "kernel/task.h"
#include "kernel/types.h"

// argv. The words are views into the shell's line buffer, which outlives the
// program; nothing here owns anything.
struct Args {
    Span<const Str> v;

    usize size() const { return v.size(); }

    Str operator[](usize i) const { return v[i]; }

    Str name() const { return v.empty() ? Str() : v[0]; }

    Args tail() const { return Args{v.subspan(1)}; }
};

// A byte sink. In M3 the only sink is the screen; M4 puts a Channel<Bytes>
// behind the same handle. A function pointer rather than a vtable, because
// there will be exactly two implementations.
struct Stream {
    using WriteFn = Result<usize> (*)(void *ctx, Str s);

    WriteFn fn = nullptr;
    void *ctx = nullptr;

    // Awaitable from day one, so M4's blocking pipe write changes no caller.
    // The work happens in await_suspend rather than await_ready because only
    // await_suspend can reach the promise, and therefore the cancel state
    // (Concept.md §8.1).
    struct Write {
        WriteFn fn;
        void *ctx;
        Str data;
        Result<usize> r = Err(Error::Invalid);

        bool await_ready() const noexcept { return false; }

        template <class P> bool await_suspend(std::coroutine_handle<P> h) {
            CancelState *c = h.promise().cancel;
            if (c && c->cancelled)
                r = Err(Error::Cancelled);
            else if (fn)
                r = fn(ctx, data);
            return false; // an M3 sink never blocks
        }

        Result<usize> await_resume() const { return r; }
    };

    Write write(Str s) const { return Write{fn, ctx, s}; }
};

// Concept.md §3.6's stdio. stdin arrives with M4: in M3 the shell owns the
// keyboard and no program reads keys.
struct Stdio {
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
#define BRAAM_PROGRAM(fn, prog_name, prog_usage)                                              \
    static Task<i32> fn(Args, Stdio);                                                         \
    static Program fn##_desc{prog_name, prog_usage, fn, nullptr};                             \
    [[maybe_unused]] __attribute__((used)) static const ProgramRegistrar fn##_reg{fn##_desc}; \
    static Task<i32> fn([[maybe_unused]] Args args, [[maybe_unused]] Stdio io)

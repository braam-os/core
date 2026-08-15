// Shell builtins: the commands that are the shell's own state rather than
// programs, and so are not files in /bin and never will be.
//
// What makes one a builtin has changed now that the shell is itself a program.
// It is no longer "no syscall could serve it" — `chdir`, `wait` and `kill` all
// exist. It is that the thing it touches is *this process's own*: `cd` moves
// the working directory a typed command inherits, `jobs`, `fg` and `kill` read
// and signal a table no syscall shows to anybody else, `exit` ends the shell's
// loop, and `help` lists the rest.
//
// A builtin runs inside the shell rather than as a child, so it reads and
// writes descriptors like anything else and pipes and redirects for free. One
// discipline goes with that: **a builtin buffers its output and writes it
// once.** Nothing in this process can wait for a sibling task, so a builtin in
// a pipeline runs to completion in its turn rather than alongside — and a pipe
// holds eight chunks, so a builtin that wrote a line at a time would fill one
// and park with nobody left to drain it.
//
// The table is an explicit sorted array rather than a list each entry registers
// itself on, for the reason it always was: --gc-sections never extracts an
// archive member nothing references.
#pragma once

#include "kernel/args.h"
#include "kernel/span.h"
#include "kernel/task.h"
#include "proc/io.h"

// The three descriptors a stage was given. Deliberately not ChildIo: those
// numbers are what a spawn *moves*, and a builtin only reads and writes them.
struct ShIo {
    u32 in  = SYS_STDIN;
    u32 out = SYS_STDOUT;
    u32 err = SYS_STDERR;
};

struct Builtin {
    Str name;
    Str usage; // one line, what `help` prints
    Task<i32> (*run)(Args, ShIo);
};

// Null when the name is not a builtin, which is when it is looked for in /bin.
const Builtin *builtin_find(Str name);

// Iteration for `help`, in name order.
Span<const Builtin> builtins();

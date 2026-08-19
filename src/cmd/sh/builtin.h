// Shell builtins, under a rule with two clauses. The first: what touches this
// process's own state — its cwd, its job table, its loop — and so is not a
// file in /bin and never will be. A shell function is the same thing named by
// the user, and is looked up first.
//
// The second: **a command whose whole cost is the spawn.** A program costs an
// instantiation and a worker, roughly a millisecond (Concept.md §4.4), and
// `while [ … ]; do echo …; done` pays it twice a turn. It admits `test`, `[`,
// `:`, `echo`, `true` and `false` — a few lines each — and nothing else.
//
// **The file in /bin stays.** A builtin shadows the name at a prompt, not
// everywhere: /bin/test is what a future `find -exec` would run, and there is
// nowhere else to put it.
//
// **A builtin buffers its output and writes it once.** It runs in its turn
// rather than alongside, and a pipe holds eight writes of any size
// (PIPE_SLOTS, ../user/io.h), so a line at a time would fill one and park with
// nobody left to drain it. See Release_Notes.md for the rest of the reasoning.
//
// The table is an explicit sorted array: --gc-sections never extracts an
// archive member nothing references.
#pragma once

#include "kernel/args.h"
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
    Task<i32> (*run)(Args, ShIo);
};

// Null when the name is not a builtin, which is when it is looked for in /bin.
// The only way in: what a builtin is *for* is a line in /share/help, which
// /bin/help pages, so the table carries no usage string to go stale.
const Builtin *builtin_find(Str name);

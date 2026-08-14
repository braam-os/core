// Shell builtins: the commands that are the shell's own state rather than
// programs, and so are not files in /bin and never will be.
//
// What makes one a builtin is that no syscall could give it to a process. `cd`
// moves the one global working directory; `jobs`, `fg` and `kill` read and
// signal the shell's job table, and `fg` claims the keyboard route of the very
// pipeline it is a stage of; `exit` ends the shell's loop; `help` lists the
// table below. Everything else is a binary.
//
// The table is an explicit sorted array rather than a registrar list like
// src/prog/'s. braam_user is a STATIC library, and --gc-sections never extracts
// an archive member nothing references, so a self-registering builtin would be
// dropped silently. Six entries named in one file cannot be.
#pragma once

#include "prog.h"

// The same shape a program has, so a builtin is an ordinary pipeline stage:
// `help | grep ls` works, `jobs > f` works, and ^C reaches it through the same
// awaitables. That is also what `fg` requires — the pump it borrows belongs to
// the pipeline it is running in.
struct Builtin {
    Str name;
    Str usage; // one line, what `help` prints
    ProgramFn run;
};

// Null when the name is not a builtin, which is when it is looked for in /bin.
const Builtin *builtin_find(Str name);

// Iteration for `help`, in name order.
Span<const Builtin> builtins();

// Running a parsed line: a pipeline of stages, the pipes between them, and the
// tty pump that watches the keyboard while they run.
//
// The stages are independent scheduler jobs rather than children the shell
// co_awaits, which is a departure from Concept.md §3.6's "a parent co_awaits a
// child group". It is forced: CancelState::waiting is a single slot, so one
// job cannot have two children parked at once, and a pipeline needs every
// stage parked at the same time. Cancellation is put back by hand, from a
// destructor in run_line's frame.
#pragma once

#include "prog.h"

// Runs one line and reports the pipeline's status, which is its last stage's —
// or 130 if ^C interrupted it. Diagnostics go to `io.err`.
Task<i32> run_line(Str line, Stdio io);

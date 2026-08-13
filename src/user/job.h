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

#include "io.h"
#include "prog.h"

// Runs one line and reports the pipeline's status, which is its last stage's —
// or 130 if ^C interrupted it. A line ending in `&` starts its pipeline, files
// it in the table below and reports 0 straight away. Diagnostics go to
// `io.err`.
Task<i32> run_line(Str line, Stdio io);

// A background job, as `jobs`, `fg` and `kill` see it. `cmd` is owned by the
// table, since the line it was typed on is a view into the shell's frame.
struct JobInfo {
    u32 id;
    u32 pid; // the first stage's
    Str cmd;
    bool running;
    i32 status; // valid once it has stopped
};

usize jobs_count();
bool jobs_at(usize i, JobInfo &out);
bool jobs_find(u32 id, JobInfo &out);

// The most recent job — what a bare `fg` means.
u32 jobs_current();

// Cancels every stage. False when there is no such job.
bool jobs_kill(u32 id);

// Where the tty pump should put what is typed while `fg` waits.
Pipe *jobs_input(u32 id);

// Parks until the job stops, and reports its status. Err(Invalid) for an
// unknown id; a job that has already stopped returns at once.
Task<Result<i32>> jobs_wait(u32 id);

// Prints and drops every job that has finished — what the shell does before
// each prompt, so a background job is announced rather than found by chance.
Task<void> jobs_report(Stream out);

// Drops the table. For tests, so a case starts from nothing.
void jobs_reset();

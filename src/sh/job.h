// Running a parsed line: a pipeline of stages, the pipes between them, and the
// job table behind `&`.
//
// Every stage that is a program is a child of this process (Concept.md §4.3):
// `pipe` makes the channels, `spawn` moves an end into each child, `wait`
// collects them, and `kill` is what `kill %n` is. What the kernel used to do
// for the shell it now does *for* the shell, through the same five operations
// any program can call.
#pragma once

#include "builtin.h"
#include "kernel/str.h"
#include "kernel/string.h"

// Runs one line and reports the pipeline's status, which is its last stage's —
// or 130 if ^C interrupted it. A line ending in `&` starts its pipeline, files
// it in the table below and reports 0 straight away.
//
// `interactive` says whether the console is this shell's to hand out: the
// keyboard is given back around a foreground pipeline and taken again after,
// and the stages are put in front so that ^C reaches them rather than us.
Task<i32> run_line(Str line, bool interactive);

// A background job, as `jobs`, `fg` and `kill` see it.
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

// Cancels every stage. Err(Invalid) when there is no such job.
Task<Result<void>> jobs_kill(u32 id);

// Puts the job in front and parks until it stops, reporting its status.
// Err(Invalid) for an unknown id; a job that has already stopped returns at
// once. `interactive` is run_line's.
Task<Result<i32>> jobs_wait(u32 id, bool interactive);

// Collects and announces every job that has finished, which the shell does
// before each prompt. Liveness is read out of /proc rather than waited for:
// nothing here may park on a job that is still running, since the prompt has to
// come back either way.
Task<void> jobs_report(u32 fd);

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

// Runs one text, which may be several lines, and reports the last pipeline's
// status — its own last stage's, or 130 if ^C interrupted it. A pipeline
// followed by `&` is started, filed in the table below and reported as 0.
//
// **130 is this shell's SIGINT.** There is no signal, and a status is the only
// thing that crosses a process boundary, so a stage reporting 130 stops the
// rest of the text. A program that exits 130 of its own accord does the same,
// which is the price of having no other channel.
//
// `interactive` says whether the console is this shell's to hand out: the
// keyboard is given back around a foreground pipeline and taken again after,
// and the stages are put in front so that ^C reaches them rather than us.
Task<i32> run_line(Str line, bool interactive);

// Whether the text ended *inside* something — a quote, a `${`, a trailing
// `&&`, an unclosed `{` — so a reader should take another line before running
// it. A parse of its own, which is what makes the accumulation re-parsing
// rather than a lexer that can block.
bool line_incomplete(Str text);

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

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

// Whether the line being run belongs to an interactive shell, which a builtin
// has no other way to ask. False outside a walk.
bool sh_interactive();

// `set -e -x -u`. Three letters of this process's own state, so they go back
// with everything else around a `( … )`.
constexpr u32 SH_ERREXIT = 1;
constexpr u32 SH_NOUNSET = 2;
constexpr u32 SH_XTRACE  = 4;

u32 sh_flags();
void sh_set_flags(u32 f);

// `trap`. Two signals exist here: 0 is EXIT and anything else is INT, since
// there are no signals and the builtin refuses every other number.
bool trap_set(u32 sig, Str action);
bool trap_get(u32 sig, Str &action);
void trap_clear(u32 sig);

// Runs a trap's action, having taken it first so the action cannot fire
// itself. 0 when there is none to run.
Task<i32> trap_run(u32 sig, bool interactive);

// `break` and `continue`, which ask rather than act for the reason `exit`
// does: a builtin runs inside a pipeline and the loop that must hear it is up
// the walk. Outside a loop both are silent no-ops.
void loop_break(u32 levels, bool cont);

// `return`, the same way. Outside a function or a sourced file it is a no-op.
void func_return(i32 status);

// `.` and `eval`: both parse a text and run it in the walk already in
// progress, which is what lets either set a variable or define a function.
Task<i32> sh_source(Str path, ShIo io);
Task<i32> sh_eval(Args args, ShIo io);

// `unset -f`. False when there is no such function.
bool func_unset(Str name);

// `exec`: with no command its redirections become the shell's own and outlive
// the line; with one it spawns, waits, and ends the shell with that status.
Task<i32> sh_exec(Args args, ShIo io);

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

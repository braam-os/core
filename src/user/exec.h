// exec: what a command name resolves to, and what it takes to run one as a
// process of its own (Concept.md §4). Every program runs in a worker of its
// own, so userland asks for a name and there is nothing else to ask for.
#pragma once

#include "kernel/string.h"
#include "kernel/sysabi.h"
#include "kernel/task.h"
#include "prog.h"

// A resolved command: a binary and its bytes. The pid is filled in by whoever
// spawns the task, and read by the task itself a tick later — a Task is lazy,
// so nothing has looked yet.
//
// For a script the binary is the interpreter's, and `lead` carries the words it
// is entered with in place of argv[0].
struct Executable {
    u32 pid   = 0;
    u32 depth = 0; // how many spawns from init this is
    String path;
    String image;
    String lead; // argv-encoded [interp, arg?, script]; empty for a binary
    ProcMeta meta{};
};

// The `braam` custom section (Concept.md §4.3). Err(Invalid) when there is none
// to read and the file is therefore not a program; Err(Unsupported) when there
// is one of ours whose `abi` is not this kernel's, which is a stale binary and
// wants saying so.
Result<ProcMeta> exec_meta(Str image);

// /bin, then the name itself once it looks like a path. Err(NotFound) is "no
// such command"; Err(Invalid) is "not executable". There is nothing to look at
// before /bin any more: a builtin is the shell's own frame, and the shell is a
// program that never asks the kernel for one.
//
// A file that is not a module is looked at once more for a `#!` line naming an
// absolute interpreter, resolved one level deep — an interpreter that is itself
// a script is refused — and `out.path` becomes that interpreter's. An
// interpreter that is not there is Err(Invalid), not Err(NotFound).
//
// `cwd` is what a name with a slash in it is relative to. Empty means the
// kernel's own, which is what init runs /bin/sh from; a Sys::Spawn passes the
// spawning process's.
Task<Result<void>> exec_resolve(Str name, Executable &out, Str cwd = Str());

// A program as its own instance. The task returned *is* the process: the
// scheduler runs it, /proc lists it, ^C cancels it, and its destructor
// terminates the worker — so a killed process needs no unwinding of its own.
//
// A host with no worker to give is waited out rather than failed: the spawn
// backs off and says so on `io.err` until one can be had (Concept.md §4).
//
// `cwd` is the directory it starts in, inherited from whoever spawned it. Empty
// means the shell's, which is what a command typed at the prompt gets.
//
// `env` is the environment blob it is entered with, in the encoding of
// sysabi.h's argv. Empty is an empty environment.
//
// `died` is false only when the process ended on its own terms, and true on
// every other way out — a trap, a step that failed, an instance that would not
// be made. A status cannot say which: `exit 132` is a program's word for what
// 132 is the kernel's word for. Init is what needs the difference (boot.cpp).
Task<i32> exec_process(Executable &exe, Args args, Stdio io, Str cwd = Str(), Str env = Str(),
                       bool *died = nullptr);

// What only the process record knows about a task, for /proc to publish. The
// scheduler has the rest; this is the half a worker comes with.
struct ProcState {
    bool worker   = false; // an instance in a worker of its own, so a process
    u32 ppid      = 0;     // 0 is nobody: init's parent, and the kernel's tasks
    u32 calls     = 0;     // asynchronous syscalls the instance is parked on
    u32 fds       = 0;     // descriptors open; stdio is not one, it is a Stdio
    u32 pages     = 0;     // pages the instance has committed, as of its last step
    u32 max_pages = 0;     // and the cap the kernel set at spawn
    bool dead     = false;
    Str cwd;
};

// False when no process record mentions the pid, which is a task the kernel runs
// for itself. A syscall server is one of those and still has a parent: it is a
// job of the process it serves, so /proc can say whose it is.
bool exec_proc_state(u32 pid, ProcState &out);

// What running processes has cost since boot, for /proc/stat. System-wide, and
// never reset: a process's own share goes with its record.
struct ExecStats {
    u64 syscalls; // parked calls, each costing a step to answer
    u64 sysfast;  // answered inside the import, Sys::Stage among them
    u64 steps;    // steps issued to a worker, two postMessage hops each
};

void exec_stats(ExecStats &out);

// The kernel's half of the two process imports, exported by main.cpp. `pid` is
// the one the host bound into that process's closure at instantiation, never
// one the process supplied: there is no argument for it on the other side.
i32 exec_sys(u32 pid, u32 op, u32 a0, u32 a1, u32 a2);
i32 exec_sys_async(u32 pid, u32 op, u32 token, u32 len);

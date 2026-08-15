// exec: what a command name resolves to, and what it takes to run one as a
// process of its own (Concept.md §4). The tier comes out of the binary's
// metadata, so userland asks for a name and never asks for a tier.
#pragma once

#include "kernel/string.h"
#include "kernel/sysabi.h"
#include "kernel/task.h"
#include "prog.h"

// A resolved command: a binary and its bytes. The pid is filled in by whoever
// spawns the task, and read by the task itself a tick later — a Task is lazy,
// so nothing has looked yet.
struct Executable {
    Tier tier = Tier::Instance;
    u32 pid   = 0;
    u32 depth = 0; // how many spawns from init this is
    String path;
    String image;
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
// `cwd` is what a name with a slash in it is relative to. Empty means the
// kernel's own, which is what init runs /bin/sh from; a Sys::Spawn passes the
// spawning process's.
Task<Result<void>> exec_resolve(Str name, Executable &out, Str cwd = Str());

// A tier-2 program as its own instance. The task returned *is* the process:
// the scheduler runs it, /proc lists it, ^C cancels it, and its destructor
// drops the instance — so a killed process needs no unwinding of its own.
//
// `cwd` is the directory it starts in, inherited from whoever spawned it. Empty
// means the shell's, which is what a command typed at the prompt gets.
//
// `died` is false only when the process ended on its own terms, and true on
// every other way out — a trap, a step that failed, an instance that would not
// be made. A status cannot say which: `exit 132` is a program's word for what
// 132 is the kernel's word for. Init is what needs the difference (boot.cpp).
Task<i32> exec_process(Executable &exe, Args args, Stdio io, Str cwd = Str(), bool *died = nullptr);

// The working directory of a live process, for /proc to publish. False when the
// pid is not one, which is every scheduler job that is not a program.
bool exec_proc_cwd(u32 pid, Str &out);

// The kernel's half of the two process imports, exported by main.cpp. `pid` is
// the one the host bound into that process's closure at instantiation, never
// one the process supplied: there is no argument for it on the other side.
i32 exec_sys(u32 pid, u32 op, u32 a0, u32 a1, u32 a2);
i32 exec_sys_async(u32 pid, u32 op, u32 token, u32 len);

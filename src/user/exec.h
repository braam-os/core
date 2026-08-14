// exec: what a command name resolves to, and what it takes to run one as a
// process of its own (Concept.md §4). The tier comes out of the binary's
// metadata, so userland asks for a name and never asks for a tier.
#pragma once

#include "builtin.h"
#include "kernel/string.h"
#include "kernel/sysabi.h"
#include "kernel/task.h"
#include "prog.h"

// A resolved command: a shell builtin, or a binary and its bytes. The pid is
// filled in by whoever spawns the task, and read by the task itself a tick
// later — a Task is lazy, so nothing has looked yet.
struct Executable {
    Tier tier              = Tier::Instance;
    const Builtin *builtin = nullptr;
    u32 pid                = 0;
    String path;
    String image;
    ProcMeta meta{};
};

// The `braam` custom section (Concept.md §4.3), or Err(Invalid) when there is
// none to read and the file is therefore not a program.
Result<ProcMeta> exec_meta(Str image);

// The builtins first, then /bin, then the name itself once it looks like a
// path. Err(NotFound) is "no such command"; Err(Invalid) is "not executable".
Task<Result<void>> exec_resolve(Str name, Executable &out);

// A tier-2 program as its own instance. The task returned *is* the process:
// the scheduler runs it, /proc lists it, ^C cancels it, and its destructor
// drops the instance — so a killed process needs no unwinding of its own.
Task<i32> exec_process(Executable &exe, Args args, Stdio io);

// The kernel's half of the two process imports, exported by main.cpp. `pid` is
// the one the host bound into that process's closure at instantiation, never
// one the process supplied: there is no argument for it on the other side.
i32 exec_sys(u32 pid, u32 op, u32 a0, u32 a1, u32 a2);
i32 exec_sys_async(u32 pid, u32 op, u32 token, u32 len);

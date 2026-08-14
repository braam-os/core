// The host's half of the process tier (Concept.md §4.3): compiling a binary,
// instantiating it, and stepping it. Only JS can call another instance's
// exports, so every one of these is a request the host performs after tick()
// has returned — the kernel is never on the stack when a process runs.
#pragma once

#include "kernel/string.h"
#include "kernel/sysabi.h"
#include "svc.h"

// Compiles `image` (cached by `path`) and instantiates it with a memory of
// `meta.initial_pages`, capped at `meta.max_pages`. Takes the image, because
// the record has to own it past a cancelled await and the caller has no more
// use for it.
Task<Result<void>> proc_spawn(u32 pid, Str path, String &&image, const ProcMeta &meta);

// One _start or _resume. `payload` is argv the first time and a syscall reply
// afterwards; the answer is what the process did with it.
Task<Result<ProcStep>> proc_step(u32 pid, Str payload);

// Drops the instance. Told, not asked: it has no reply, and it is issued from
// a destructor, where there is nothing left to await with.
void proc_kill(u32 pid);

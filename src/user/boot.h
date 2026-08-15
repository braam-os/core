// Bringing the system up: the mounts, and then the shell.
//
// init spawns the console pump and one task, and this is that task. The shell
// used to do the mounting itself and cannot any more — it is a file in /bin,
// and /bin is one of the mounts.
#pragma once

#include "kernel/task.h"
#include "kernel/types.h"

// The one program init runs. There is no getty and no /etc/inittab: init starts
// another when this one *dies*, and the terminal is done when it exits.
constexpr Str SHELL = "/bin/sh";

// Printed on the grid before that first prompt, and absent is not an error: a
// boot archive without a greeting is not a broken one.
constexpr Str MOTD = "/share/motd";

Task<void> boot_filesystem();

// `pid` is init's own, which is the shell's: it runs inside init's task rather
// than a job of its own. Read a tick after this is called, so the caller may
// fill it in once sched_spawn has told it what the pid is.
Task<i32> init_task(const u32 &pid);

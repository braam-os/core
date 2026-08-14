// The shell: read a line, tokenise it, look the program up, run it. Spawned
// by init() as the kernel's only boot task.
#pragma once

#include "kernel/task.h"
#include "kernel/types.h"

Task<i32> shell();

// What the `exit` builtin asks for. It cannot end the loop itself: a builtin is
// a pipeline stage, and the shell is parked collecting its report when it runs.
// The request is honoured when run_line returns, at the next prompt.
void shell_exit(i32 status);

// The pending request, if there is one, and clears it.
bool shell_exit_wanted(i32 &status);

// The prompt loop: what /bin/sh is.
#pragma once

#include "kernel/task.h"
#include "kernel/types.h"

// Reads lines and runs them until `exit`, end of input, or a failure it cannot
// come back from. Reports the status the shell leaves with.
//
// `interactive` is whether the console is ours: with it, the shell holds the
// keyboard at the prompt and hands it to whatever it runs. Without it — a shell
// reading a script off a pipe — it reads stdin a line at a time and touches
// neither the keyboard nor the foreground.
Task<i32> shell(bool interactive);

// `exit`, which asks rather than acts: a builtin runs inside a line, and the
// loop reads the request when that line is finished.
void shell_exit(i32 status);

bool shell_exit_wanted(i32 &status);

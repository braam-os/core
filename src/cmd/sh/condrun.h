// The half of `test` that has to go and look: it answers cond.h's probes out
// of the filesystem and reports the expression through an exit status.
//
// It touches no shell state and reaches nothing but proc/io.h, which is what
// lets /bin/test link it beside the builtin that shadows the name.
#pragma once

#include "kernel/args.h"
#include "kernel/task.h"
#include "kernel/types.h"

// 0 true, 1 false, 2 a malformed expression — reported as `test: why` on
// `errfd`. `a` is the expression alone: no command name, and no closing `]`.
Task<i32> cond_run(Args a, u32 errfd);

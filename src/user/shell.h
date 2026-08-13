// The shell: read a line, tokenise it, look the program up, run it. Spawned
// by init() as the kernel's only boot task.
#pragma once

#include "kernel/task.h"
#include "kernel/types.h"

Task<i32> shell();

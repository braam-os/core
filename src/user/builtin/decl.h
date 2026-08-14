// The bodies behind the table in table.cpp, one file each. They are named
// rather than self-registering for the reason builtin.h gives.
#pragma once

#include "user/prog.h"

Task<i32> builtin_cd(Args args, Stdio io);
Task<i32> builtin_exit(Args args, Stdio io);
Task<i32> builtin_fg(Args args, Stdio io);
Task<i32> builtin_help(Args args, Stdio io);
Task<i32> builtin_jobs(Args args, Stdio io);
Task<i32> builtin_kill(Args args, Stdio io);

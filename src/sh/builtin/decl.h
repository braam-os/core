// One declaration per builtin, so table.cpp names them without a header each.
#pragma once

#include "sh/builtin.h"

Task<i32> builtin_cd(Args args, ShIo io);
Task<i32> builtin_exit(Args args, ShIo io);
Task<i32> builtin_fg(Args args, ShIo io);
Task<i32> builtin_help(Args args, ShIo io);
Task<i32> builtin_jobs(Args args, ShIo io);
Task<i32> builtin_kill(Args args, ShIo io);

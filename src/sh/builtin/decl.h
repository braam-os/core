// One declaration per builtin, so table.cpp names them without a header each.
#pragma once

#include "sh/builtin.h"

Task<i32> builtin_break(Args args, ShIo io);
Task<i32> builtin_cd(Args args, ShIo io);
Task<i32> builtin_continue(Args args, ShIo io);
Task<i32> builtin_dot(Args args, ShIo io);
Task<i32> builtin_eval(Args args, ShIo io);
Task<i32> builtin_exit(Args args, ShIo io);
Task<i32> builtin_export(Args args, ShIo io);
Task<i32> builtin_fg(Args args, ShIo io);
Task<i32> builtin_help(Args args, ShIo io);
Task<i32> builtin_jobs(Args args, ShIo io);
Task<i32> builtin_kill(Args args, ShIo io);
Task<i32> builtin_readonly(Args args, ShIo io);
Task<i32> builtin_return(Args args, ShIo io);
Task<i32> builtin_set(Args args, ShIo io);
Task<i32> builtin_shift(Args args, ShIo io);
Task<i32> builtin_unset(Args args, ShIo io);

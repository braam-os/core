// One declaration per builtin, so table.cpp names them without a header each.
#pragma once

#include "cmd/sh/builtin.h"

Task<i32> builtin_bracket(Args args, ShIo io);
Task<i32> builtin_break(Args args, ShIo io);
Task<i32> builtin_cd(Args args, ShIo io);
Task<i32> builtin_colon(Args args, ShIo io);
Task<i32> builtin_continue(Args args, ShIo io);
Task<i32> builtin_dot(Args args, ShIo io);
Task<i32> builtin_echo(Args args, ShIo io);
Task<i32> builtin_eval(Args args, ShIo io);
Task<i32> builtin_exec(Args args, ShIo io);
Task<i32> builtin_exit(Args args, ShIo io);
Task<i32> builtin_export(Args args, ShIo io);
Task<i32> builtin_false(Args args, ShIo io);
Task<i32> builtin_fg(Args args, ShIo io);
Task<i32> builtin_help(Args args, ShIo io);
Task<i32> builtin_jobs(Args args, ShIo io);
Task<i32> builtin_kill(Args args, ShIo io);
Task<i32> builtin_read(Args args, ShIo io);
Task<i32> builtin_readonly(Args args, ShIo io);
Task<i32> builtin_return(Args args, ShIo io);
Task<i32> builtin_set(Args args, ShIo io);
Task<i32> builtin_shift(Args args, ShIo io);
Task<i32> builtin_test(Args args, ShIo io);
Task<i32> builtin_trap(Args args, ShIo io);
Task<i32> builtin_true(Args args, ShIo io);
Task<i32> builtin_unset(Args args, ShIo io);
Task<i32> builtin_wait(Args args, ShIo io);

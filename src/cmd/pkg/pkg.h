// /bin/pkg: the subcommand table, and the dispatch over it.
#pragma once

#include "kernel/args.h"
#include "kernel/str.h"
#include "kernel/task.h"
#include "kernel/types.h"

// `run` is null until the task that builds the command lands.
struct PkgCommand {
    Str name;
    Task<i32> (*run)(Args args);
};

// argv as the process was entered with it: argv[0] is `pkg`.
Task<i32> pkg_run(Args args);

// The subcommands, one file each. argv[0] is the subcommand's own name.
Task<i32> pkg_update(Args args);

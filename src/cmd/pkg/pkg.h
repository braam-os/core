// /bin/pkg: the subcommand table, and the dispatch over it.
#pragma once

#include "index.h"
#include "kernel/args.h"
#include "kernel/str.h"
#include "kernel/task.h"
#include "kernel/types.h"

// Every row carries a command; `run` is checked because a null one is a row
// added without its function, not a state the table is in.
struct PkgCommand {
    Str name;
    Task<i32> (*run)(Args args);
};

// argv as the process was entered with it: argv[0] is `pkg`.
Task<i32> pkg_run(Args args);

// The subcommands, one file each. argv[0] is the subcommand's own name.
Task<i32> pkg_update(Args args);
Task<i32> pkg_install(Args args);
Task<i32> pkg_remove(Args args);
Task<i32> pkg_autoremove(Args args);
Task<i32> pkg_upgrade(Args args);

// The three that only read, in query.cpp.
Task<i32> pkg_search(Args args);
Task<i32> pkg_info(Args args);
Task<i32> pkg_list(Args args);

// The two that read §8.1's record, in verify.cpp.
Task<i32> pkg_files(Args args);
Task<i32> pkg_verify(Args args);

// The collector, in clean.cpp.
Task<i32> pkg_clean(Args args);

// /pkg/index as an update left it, unchecked (§7's checks are not repeated).
// 0 is a read index; anything else is the exit status, its refusal printed.
Task<i32> pkg_load_index(CheckedIndex &c);

// The active generation's packages file. An empty `path` is no generation.
Task<Result<void>> pkg_generation(String &path, String &text);

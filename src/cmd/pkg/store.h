// The half of the store that needs syscalls, kept out of db.cpp so that one
// compiles into tests.wasm.
#pragma once

#include "db.h"
#include "kernel/result.h"
#include "kernel/task.h"
#include "stanza.h"

// Performs a list db.h computed, stopping at the first failure.
Task<Result<void>> store_perform(Span<const StoreOp> ops);

// A whole file, replacing what is there. An unpack writes an entry and frees
// it; a StoreOp per entry would hold the whole package.
Task<Result<void>> store_write(Str path, Str bytes);

// The live generation, or 0. /pkg/active is a symlink, so read_link.
Task<Result<u32>> store_active();

// A whole small file. Err(NotFound) comes back as empty: a tree that never had
// a world or a repositories list has nothing in one.
Task<Result<String>> store_slurp(Str path);

// The commands /pkg/store/<stem>/bin holds, sorted. No bin/ is not an error.
Task<Result<Vec<String>>> store_commands(Str name, Str version);

// /pkg/db/<name>-<version>. The reader hands back the file, since a DbRecord
// views it.
Task<Result<void>> store_db_put(const DbRecord &r);
Task<Result<String>> store_db_get(Str name, Str version);

// Drops a store directory: Sys::Remove's recursive bit, not a walk.
Task<Result<void>> store_drop(Str name, Str version);

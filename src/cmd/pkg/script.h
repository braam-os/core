// §5.1's install scripts: an ordinary /bin/sh process, with the authority of
// whoever typed the command (Package_Management.md §11).
#pragma once

#include "kernel/result.h"
#include "kernel/span.h"
#include "kernel/str.h"
#include "kernel/task.h"
#include "kernel/types.h"
#include "zip.h"

// The script out of /pkg/store/<name>-<version>/, run to completion. Its exit
// status, or 0 where the package carries none. `args` is argv[1] onward: §5.1's
// versions for the six, and the matched directories for a trigger.
Task<Result<i32>> script_run(Str name, Str version, ZipMeta kind, Span<const Str> args);

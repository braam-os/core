// §5.1's triggers: apk's fire_triggers, which directories a package's `g:`
// globs wake it for and which of them it is handed.
//
// Syscall-free. Every Str views what the caller holds.
#pragma once

#include "kernel/span.h"
#include "kernel/str.h"
#include "kernel/types.h"
#include "kernel/vec.h"

// One directory the transaction has a view of.
struct TriggerDir {
    Str path;
    bool modified = false; // this transaction wrote it
};

// A glob against a path, component by component: `*` does not cross a `/`.
bool trigger_match(Str pattern, Str path);

// `fire` is whether .trigger runs, `out` the directories it is handed — which
// may be empty, a `+` glob waking it without contributing one. `fresh` is a
// package this transaction installed or upgraded.
bool trigger_dirs(Str globs, bool fresh, Span<const TriggerDir> dirs, Vec<Str> &out, bool &fire);

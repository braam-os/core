// The half of the anchor that needs a syscall, kept out of trust.cpp so that
// one compiles into tests.wasm.
#pragma once

#include "kernel/result.h"
#include "kernel/task.h"
#include "trust.h"

// Shipped in rootfs.zip, re-pinned by the boot unpack (§6).
constexpr Str ANCHOR_PATH = "/share/pkg/anchor";

// §7 step 2: missing is Err(NotFound), unreadable Err(Invalid), one that does
// not meet its own root threshold at `now` Err(Perm). All three stop.
Task<Result<void>> anchor_load(u64 now, AnchorFile &out);

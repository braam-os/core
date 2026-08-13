// The kernel's side of the storage ABI (Concept.md §5.2, §5.3).
//
// The request machinery itself is generic and lives in kernel/hostcall.h; what
// is here is the operations storage names on it.
#pragma once

#include "fs.h"
#include "kernel/hostcall.h"

enum class FsOp : u32 {
    Info = 1, // the StorageBackend, into buf
    Bundle,   // the boot archive, into buf
    Open,     // path, flags -> result = handle
    Stat,     // path -> flags = NodeKind, result = size
    List,     // path -> packed entries, into buf
    Mkdir,    // path
    Remove,   // path, flags bit 0 = recursive
};

// host_fs_sync's operations. All of them act on an already-open handle.
enum class FsSyncOp : u32 {
    Read = 1, // -> bytes read
    Write,    // -> bytes written
    Size,     // -> size, which therefore cannot exceed 2 GiB
    Truncate,
    Flush,
    Close,
};

// A storage operation. The record's inline string argument is the path.
struct FsCall : HostCall {
    FsCall(FsOp op, Str path, u32 flags) : HostCall(HostIface::Fs, u32(op), path, flags) {}
};

// Capabilities and usage, straight from the host (Concept.md §5.3).
Task<Result<StorageBackend>> storage_info();

// The boot archive BundleFs parses, or Err(NotFound) when the host has none.
Task<Result<String>> storage_bundle();

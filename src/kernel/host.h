// Imports from the JS host. Every one is non-blocking (Concept.md §2.2);
// host_now is one of the two sanctioned value-returning exceptions.
#pragma once

#include "str.h"
#include "types.h"

BRAAM_IMPORT("log") void host_log(const char *ptr, usize len);
BRAAM_IMPORT("now") f64 host_now();

// The damage rectangle; the renderer reads the cells themselves out of linear
// memory (Concept.md §3.5). Notifies, and so returns nothing.
BRAAM_IMPORT("present") void host_present(u32 x, u32 y, u32 w, u32 h);

// Storage (Concept.md §5). One multiplexed import per calling convention
// rather than one per operation, which is the shape §4.3 fixes for the process
// ABI; `req` addresses the HostRequest in kernel/hostcall.h that carries the
// arguments and receives the reply.
BRAAM_IMPORT("fs") void host_fs(u32 op, u32 token, u32 req);

// The second sanctioned exception to §2.2: once an OPFS sync access handle
// exists, these are genuinely synchronous and no promise is involved (§5.2).
// Returns a byte count, or a negative Error.
BRAAM_IMPORT("fs_sync") i32 host_fs_sync(u32 op, u32 handle, u32 ptr, u32 len, u32 off);

// Host services: fetch, WebSocket, clipboard, file transfer, wall clock
// (Concept.md §3.7). `req` is the same HostRequest the storage import takes;
// `ref` is the JS object the operation acts on, read out of the externref
// table, so the host never has to index the table itself.
BRAAM_IMPORT("svc") void host_svc(u32 op, u32 token, u32 req, __externref_t ref);

inline void log(Str s)
{
    host_log(s.data(), s.size());
}

[[noreturn]] inline void panic(Str s)
{
    host_log(s.data(), s.size());
    __builtin_trap();
}

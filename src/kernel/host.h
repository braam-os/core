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

inline void log(Str s) {
    host_log(s.data(), s.size());
}

[[noreturn]] inline void panic(Str s) {
    host_log(s.data(), s.size());
    __builtin_trap();
}

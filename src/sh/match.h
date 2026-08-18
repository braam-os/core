// The shell's pattern matcher: `*`, `?`, `[a-z]` and `[!…]`. Pure — Str and
// Span and nothing else, which is what lets tests.wasm compile it. Globbing
// and `case` are its two callers.
#pragma once

#include "kernel/span.h"
#include "kernel/str.h"

// Whether `name` matches `pattern`. `mark` is expand.h's quoting mask, one
// byte per byte of the pattern; a marked metacharacter matches itself. Past
// the mask's end is unmarked.
bool glob_match(Str pattern, Span<const u8> mark, Str name);

// Whether the pattern holds a live metacharacter — what tells the directory
// walk a component is worth a listing. `case` never asks.
bool glob_meta(Str pattern, Span<const u8> mark);

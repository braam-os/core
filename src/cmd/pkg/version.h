// apk's version grammar (Package_Formats.md §7), unchanged:
//
//     digit{.digit}...{letter}{_suf{#}}...{~hash}{-r#}
//
// A comparison answers one result bit; an operator is a mask of them, which is
// what collapses the spellings into one match().
#pragma once

#include "kernel/str.h"
#include "kernel/types.h"

constexpr u32 VER_EQUAL    = 1;
constexpr u32 VER_LESS     = 2;
constexpr u32 VER_GREATER  = 4;
constexpr u32 VER_FUZZY    = 8;
constexpr u32 VER_CONFLICT = 16; // inverts the answer (dep.h's `!`)
constexpr u32 VER_ANY      = VER_EQUAL | VER_LESS | VER_GREATER;

bool version_valid(Str v);

// One of VER_LESS, VER_EQUAL, VER_GREATER.
u32 version_compare(Str a, Str b);

// "<=" is VER_LESS|VER_EQUAL, "~" is VER_FUZZY|VER_EQUAL. Zero on any
// character that is not < > = ~.
u32 version_mask(Str op);

bool version_match(Str a, u32 mask, Str b);

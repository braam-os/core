// Dependencies (Package_Formats.md §6): [!]name[[op]ver], in a space- or
// newline-separated list. The operator is a mask of acceptable results, so the
// nine spellings are version.h's bitfield and not nine cases.
#pragma once

#include "kernel/str.h"
#include "kernel/types.h"
#include "version.h"

// name and version view whoever owns the text.
struct Dep {
    Str name;
    Str version;           // empty when the token named none
    u32 mask    = VER_ANY; // VER_CONFLICT for a leading `!`
    bool broken = false;   // the version did not parse: nothing satisfies it
};

enum class DepParse {
    Ok,
    Broken,   // named, but its version is unreadable
    Malformed // not a dependency at all; the record does not read
};

// Ok and Broken fill `out`; Malformed leaves it alone.
DepParse dep_parse(Str spec, Dep &out);

// The next token off `rest`, runs of separators collapsing. False at the end.
bool dep_next(Str &rest, Str &spec);

// `version` is the candidate's. False when `d` is broken.
bool dep_satisfied(const Dep &d, Str version);

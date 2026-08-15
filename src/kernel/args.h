// argv, which is the one type both sides of the process boundary need to name.
// It sat in src/user/prog.h and again in src/proc/rt.h, identically, because a
// shell builtin and a program are entered with the same thing; the shell became
// a program and the two copies became one.
//
// The words are views into whoever owns the bytes — the shell's word store for
// a pipeline stage, the process's own argv blob for a binary. Nothing here owns
// anything.
#pragma once

#include "span.h"
#include "str.h"
#include "types.h"

struct Args {
    Span<const Str> v;

    usize size() const { return v.size(); }

    Str operator[](usize i) const { return v[i]; }

    Str name() const { return v.empty() ? Str() : v[0]; }

    Args tail() const { return Args{ v.subspan(1) }; }
};

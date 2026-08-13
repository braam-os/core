// Paths are absolute, '/'-separated, and never end in a slash except for the
// root itself. Resolution happens once, at the VFS edge; nothing below it ever
// sees a relative path or a '.' component.
#pragma once

#include "kernel/result.h"
#include "kernel/str.h"
#include "kernel/string.h"

// Resolves `p` against `cwd` and normalises the result: '.' drops, '..' pops,
// repeated and trailing slashes go. A '..' at the root stays at the root.
Result<void> path_resolve(Str cwd, Str p, String &out);

// Everything before the last '/', or "/" when there is nothing.
Str path_dirname(Str p);

// Everything after the last '/'. The root's basename is "/".
Str path_basename(Str p);

// Appends `name` to `dir` with exactly one separator between them.
Result<void> path_join(Str dir, Str name, String &out);

// Whether `p` is `prefix` or lies beneath it. "/" is a prefix of everything.
bool path_under(Str prefix, Str p);

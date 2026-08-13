// Line to argv. Quoting, escaping, pipes and redirection are one grammar and
// arrive together in M4; this splits on whitespace and nothing else.
#pragma once

#include "kernel/result.h"
#include "kernel/str.h"
#include "kernel/vec.h"

// Appends the words of `line` to `argv`. They are views into `line`, which
// must outlive them; nothing is copied. Runs of separators collapse.
Result<void> tokenize(Str line, Vec<Str> &argv);

// Turning a raw word from the lexer into the fields a command sees: quote
// removal, and nothing else yet. A step between the parse and the run.
#pragma once

#include "kernel/result.h"
#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/vec.h"

// One expanded word. `mark` is one byte per byte of `text`, 1 where that byte
// came from inside quotes or from behind a backslash. Field splitting and
// globbing are its only readers.
struct Field {
    String text;
    Vec<u8> mark;
};

// Appends the fields `raw` yields — one, for now. Err(Invalid) is a quote the
// lexer would have refused, so it means the two disagree.
Result<void> expand_word(Str raw, Vec<Field> &out);

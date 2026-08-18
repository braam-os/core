// The shell's lexer. Quoting, escaping, `|` and the redirection operators are
// one grammar, so they arrive together; the parser above this is in parse.h.
//
// A word arrives raw — quotes and backslashes still in it. Removing them is
// expand.h's, one step later.
#pragma once

#include "kernel/result.h"
#include "kernel/str.h"

enum class Tok : u8 {
    End,       // the line is exhausted
    Word,      //
    Pipe,      // |
    Amp,       // &, and only at the end of a line
    Less,      // <
    Great,     // >
    DGreat,    // >>
    ErrGreat,  // 2>
    ErrDGreat, // 2>>
};

struct Lexer {
    explicit Lexer(Str line) : s_(line) {}

    // Sets `word` to a view of the line when the result is Tok::Word, and to
    // nothing otherwise. The only error is Err(Invalid), for a quote that is
    // never closed.
    Result<Tok> next(Str &word);

private:
    Str s_;
    usize i_ = 0;
};

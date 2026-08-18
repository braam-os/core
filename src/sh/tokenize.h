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

// Where the `${…}` beginning at s[at] — which must be the `$` — ends: the index
// of its closing `}`, or Str::npos. Nesting counts and a quoted run is skipped,
// so a space or a `|` in the body does not end the word. The lexer and the
// expander have to agree about that, so they share this rather than each
// carrying a copy, as they do for the quotes.
usize brace_end(Str s, usize at);

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

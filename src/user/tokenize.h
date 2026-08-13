// The shell's lexer. Quoting, escaping, `|` and the redirection operators are
// one grammar, so they arrive together; the parser above this is in parse.h.
//
// A word with its quotes removed is not a substring of the line, which is why
// next() fills a caller-owned String instead of returning a view.
#pragma once

#include "kernel/result.h"
#include "kernel/str.h"
#include "kernel/string.h"

enum class Tok : u8 {
    End,       // the line is exhausted
    Word,      //
    Pipe,      // |
    Less,      // <
    Great,     // >
    DGreat,    // >>
    ErrGreat,  // 2>
    ErrDGreat, // 2>>
};

struct Lexer {
    explicit Lexer(Str line) : s_(line) {}

    // Clears `word` and fills it when the result is Tok::Word. The only error
    // is Err(Invalid), for a quote that is never closed.
    Result<Tok> next(String &word);

private:
    Str s_;
    usize i_ = 0;
};

// Turning a raw word from the lexer into the fields a command sees: quote
// removal, `$` expansion, and splitting against IFS. A step between the parse
// and the run.
#pragma once

#include "kernel/result.h"
#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/vec.h"

// One expanded word. `mark` is one byte per byte of `text`, 1 where that byte
// came from inside quotes or from behind a backslash; what an unquoted
// expansion contributed is unmarked. Globbing and `case` are its only readers.
//
// `quoted` says quoting is why the field exists: with x empty, `"$x"` is one
// argument and `$x` is none, and both are a zero-length text and mark.
struct Field {
    String text;
    Vec<u8> mark;
    bool quoted = false;
};

// What a `$` needs from the shell. Function pointers, so the unit tests pass
// literals and this file stays clear of every syscall. All five are required.
struct Vars {
    void *ctx = nullptr;

    // A named variable, and the specials `?`, `$`, `!`, `#` and IFS under
    // their own names. False when the name is not set, which `${x-y}` and
    // `${x?y}` must tell apart from set-and-empty. The value is a view the
    // walk copies before it looks anything else up.
    bool (*look)(void *ctx, Str name, Str &value) = nullptr;

    // `${x=y}`. False is out of memory or a readonly variable.
    bool (*set)(void *ctx, Str name, Str value) = nullptr;

    // Positional parameters. `count` excludes $0; `at(0)` is $0, and anything
    // past `count` is empty.
    usize (*count)(void *ctx)     = nullptr;
    Str (*at)(void *ctx, usize i) = nullptr;

    // What the `$(…)` or backtick beginning at byte `at` of the raw word
    // printed. False when it has not been run yet, which abandons the word
    // with Err(Again) rather than guessing: running a command needs a
    // co_await, and nothing in this file may have one.
    bool (*substitute)(void *ctx, usize at, Str command, Str &out) = nullptr;

    // `set -u`: an unset parameter is Err(Invalid) rather than an empty word.
    // A field and not a callback, because only the walk below knows that
    // `${x-y}` and `${x+y}` have already asked and are exempt.
    bool nounset = false;
};

// Whether the word may become more than one. `One` is an assignment's value or
// a redirection target: exactly one field, however it expands.
enum class Split : u8 { Fields, One };

// Why Err(Invalid), and what Err(Again) is waiting for. `name` is set only by
// `${x?…}`; every view here is into the word or into a literal, so none
// outlives the call.
struct ExpandErr {
    Str name;
    Str message;

    // Err(Again): the substitution to run before this word can expand.
    usize at = 0; // where it begins in the raw word, which keys the memo
    Str command;  // the text inside it
    bool ticked = false;
};

// Appends the fields `raw` yields — none, one, or several. Err(Invalid) is a
// quote or a `${` the lexer would have refused, an expansion nested too deep,
// a malformed `${…}`, or `${x?…}` on an unset variable; `err` says which.
// Err(Again) is a substitution whose output `substitute` did not have: run it,
// remember it under `err.at`, and call again. Nothing is appended in that
// case, so a caller retrying must expand into a vector it can clear.
Result<void> expand_word(Str raw, const Vars &v, Split split, Vec<Field> &out, ExpandErr &err);

// One command line: bundled short flags, `--` to end them, and a flag that
// takes a value. Options come first and the first operand ends them; no
// permutation and no long options.
//
// Allocates nothing — every Str views the argv the process was entered with —
// and is a translation unit of its own, so a binary that never calls it does
// not carry it.
#pragma once

#include "kernel/args.h"
#include "kernel/result.h"
#include "kernel/str.h"

// What a program declares: the letters it takes, and which of those consume a
// value.
struct Opts {
    Str flags;  // "1CRSdhlr"
    Str valued; // "n" — takes the rest of the word, or the next one
};

// One flag. `value` is empty unless the letter is in Opts::valued; on an error
// `name` is the letter at fault.
struct Opt {
    char name = 0;
    Str value;
};

// A cursor over argv, starting at argv[1]. The spec is held by value.
struct OptParse {
    OptParse(Args args, Opts spec) : args_(args), spec_(spec) {}

    OptParse(const OptParse &)            = delete;
    OptParse &operator=(const OptParse &) = delete;

    // ok(true) with `out` set to the next flag; ok(false) once the operands
    // begin. Err(Invalid) is a letter the program does not take, Err(NotFound)
    // a valued letter with nothing after it.
    Result<bool> next(Opt &out);

    // The operands, once next() has reported false.
    Args rest() const { return Args{ args_.v.subspan(at_) }; }

private:
    Args args_;
    Opts spec_;
    usize at_ = 1; // the word being read
    usize in_ = 0; // how far into that word's bundle; 0 = not started
};

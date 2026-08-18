// The `test` expression, and nothing that goes and looks: v7's grammar
// evaluated over answers a caller already has. Pure — no syscall — so test/
// compiles it into tests.wasm the way it does parse.cpp and expand.cpp.
//
// A file primary needs a co_await and this grammar is five mutually recursive
// functions, so the awaits are lifted out of the recursion: cond_probes names
// every primary in one walk, the caller answers them, and cond_eval walks
// again reading the answers back. Two walks of the same code, not two
// scanners — only the grammar can say which `-f` is an operator and which is a
// word.
#pragma once

#include "kernel/args.h"
#include "kernel/span.h"
#include "kernel/vec.h"

// One file primary: the operator's letter, and the word after it. `-t` with no
// operand records "1", which is the descriptor v7 tests by default.
struct CondProbe {
    char op; // 'r' 'w' 'x' 'f' 'd' 's' 't'
    Str arg;
};

// Why the expression is neither true nor false. A view into a literal, so it
// outlives the call.
struct CondErr {
    Str message;
};

// The primaries `a` will ask about, in the order cond_eval reads them. False
// is out of memory; a malformed expression is not reported here, since
// cond_eval says the same thing with the answers in hand.
bool cond_probes(Args a, Vec<CondProbe> &out);

// True with `value` set, or false with `err` — which is a syntax error, and
// neither true nor false. `answers` is one byte per probe, in cond_probes'
// order; a missing one reads as false.
bool cond_eval(Args a, Span<const u8> answers, bool &value, CondErr &err);

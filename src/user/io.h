// Pipes, and the glue that puts one behind a Stdio handle. A pipe carries
// owning chunks: Bytes is a Span, and the writer's buffer does not outlive the
// handover to a reader that runs later.
#pragma once

#include "kernel/channel.h"
#include "kernel/string.h"
#include "prog.h"

// Chunks, not bytes: backpressure counts writes, so one huge write is one
// slot. Eight is enough to keep a producer and a consumer both busy.
constexpr usize PIPE_SLOTS = 8;

using Pipe = Channel<String, PIPE_SLOTS>;

Stream pipe_sink(Pipe &p);

Source pipe_source(Pipe &p);

// A source that is already at end of input, for a program the shell gave
// nothing to read.
Source null_source();

// Writes all of `s`, retrying the stray wake that leaves a write unfinished.
Task<Result<void>> write_all(Stream out, Str s);

// Splits a source into lines. A line may span any number of chunks, and a
// final fragment with no newline is a line.
struct LineReader {
    explicit LineReader(Source in) : in_(in) {}

    // ok(true) with `out` set to the next line, without its newline; ok(false)
    // at end of input.
    Task<Result<bool>> next(String &out);

private:
    Source in_;
    String buf_;
    usize pos_ = 0; // consumed prefix of buf_, compacted when it refills
    bool eof_  = false;
};

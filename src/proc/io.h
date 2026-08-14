// What a process's stdio looks like from inside: syscalls, and the few
// helpers a program would otherwise write again. It mirrors src/user/io.h so a
// program ported from the registry to a binary keeps its text and its exit
// codes; it is much shorter, because everything here is asynchronous and none
// of the Stream/Source machinery is needed to bridge a synchronous read.
#pragma once

#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/task.h"
#include "kernel/vec.h"
#include "rt.h"

// Writes all of `s`, retrying a short write.
Task<Result<void>> write_all(u32 fd, Str s);

// One chunk, or Err(Closed) at end of input.
Task<Result<String>> read_chunk(u32 fd);

Task<Result<i32>> open_read(Str path);

Task<void> close_fd(u32 fd);

// A diagnostic on stderr: "who: what: why".
Task<void> errln(Str who, Str what, Error why);

// The files named on a command line, read end to end as one stream — `wc a b`.
// Unlike the applet version they are opened lazily, since a read here is a
// syscall either way and nothing has to be ready before the first one.
struct Input {
    Input(Args paths, u32 fallback) : paths_(paths), fd_(fallback) {}

    Input(const Input &)            = delete;
    Input &operator=(const Input &) = delete;

    // Opens every path up front so a missing file is reported before any
    // output, which is what the applet did. Returns 0, or the exit status.
    Task<i32> open_all(Str who);

    // The next chunk of the concatenation, or Err(Closed) at the end of it.
    Task<Result<String>> read();

private:
    Args paths_;
    Vec<i32> fds_;
    usize at_ = 0;
    u32 fd_; // stdin, when no path was named
    bool own_ = false;
};

// Splits an Input into lines. The applet's twin in src/user/io.h, kept the same
// shape so a ported program's loop is the loop it had: a line may span any
// number of chunks, and a final fragment with no newline is a line.
struct LineReader {
    explicit LineReader(Input &in) : in_(in) {}

    // ok(true) with `out` set to the next line, without its newline; ok(false)
    // at end of input.
    Task<Result<bool>> next(String &out);

private:
    Input &in_;
    String buf_;
    usize pos_ = 0; // consumed prefix of buf_, compacted when it refills
    bool eof_  = false;
};

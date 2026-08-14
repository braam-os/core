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

// Opens `path` with SYS_O_* flags (sysabi.h). The flags ride in the op word
// rather than the payload, so the payload is the path and nothing else.
Task<Result<i32>> open_at(Str path, u32 flags);

Task<Result<i32>> open_read(Str path);

// A whole file, read through the syscalls. Small files only, which is what
// /proc holds — and /proc is where the kernel publishes what a process would
// otherwise need an operation of its own for.
Task<Result<String>> read_file(Str path);

// Cutting up what /proc hands back. Both advance `rest` past what they return
// and neither allocates, so a program reads a table without a parser.
bool next_line(Str &rest, Str &line);

Str next_field(Str &line);

Task<void> close_fd(u32 fd);

// What Sys::Stat answers with, and one entry of what Sys::List answers with.
// `kind` is SYS_KIND_FILE or SYS_KIND_DIR.
struct FileInfo {
    u32 kind = SYS_KIND_FILE;
    u64 size = 0;
};

struct DirEntry {
    String name;
    u32 kind = SYS_KIND_FILE;
    u64 size = 0;
};

Task<Result<FileInfo>> stat_of(Str path);

Task<Result<Vec<DirEntry>>> list_dir(Str path);

Task<Result<void>> make_dir(Str path);

Task<Result<void>> remove_path(Str path, bool all);

// What `df` reports (Concept.md §5.3). `known` is false when the host would
// not say, which is not the same as a quota of zero.
struct StorageInfo {
    u64 quota   = 0;
    u64 usage   = 0;
    bool opfs      = false;
    bool sync      = false;
    bool persisted = false;
    bool known     = false;
};

Task<Result<StorageInfo>> storage_of();

// Parks for `ms`, on the kernel's timer queue. Err(Cancelled) on ^C.
Task<Result<void>> sleep_for(u32 ms);

// The wall clock (Concept.md §6): milliseconds since the epoch, and the
// browser's offset from UTC. Sys::Now is monotonic and cannot name a day.
struct Clock {
    u64 epoch_ms = 0;
    i32 tz_min   = 0;
};

Task<Result<Clock>> clock_now();

// Host services. Everything that is a stream of bytes comes back as a
// descriptor, so read_chunk and close_fd serve it and there is nothing new to
// learn: a fetched body reads like a file, and a socket is written like one.

// A fetch whose headers have arrived. `body` is read with read_chunk until it
// reports Err(Closed), and closed with close_fd.
struct Fetched {
    u32 status = 0;
    String headers;
    i32 body = -1;
};

// `spec` is the method, a blank line, any request headers, a blank line, then
// the body — the shape web/svc.js parses.
Task<Result<Fetched>> fetch_url(Str url, Str spec);

// A socket: write_all sends a message, read_chunk receives one, and an empty
// read is the peer having gone.
Task<Result<i32>> ws_connect(Str url);

Task<Result<String>> clip_get(bool wait);

Task<Result<void>> clip_put(Str text);

// The files the user chose. Each is opened by index with pick_open.
struct Chosen {
    i32 set = -1;
    Vec<String> names;
};

Task<Result<Chosen>> pick();

Task<Result<i32>> pick_open(const Chosen &c, usize index);

// Hands the bytes to the browser as a download.
Task<Result<void>> save(Str name, Str bytes);

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

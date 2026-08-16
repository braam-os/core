// Pipes, and the glue that puts one behind a Stdio handle. A pipe carries
// owning chunks: Bytes is a Span, and the writer's buffer does not outlive the
// handover to a reader that runs later.
#pragma once

#include "fs/vfs.h"
#include "kernel/channel.h"
#include "kernel/string.h"
#include "kernel/vec.h"
#include "prog.h"

// Chunks, not bytes: backpressure counts writes, so one huge write is one
// slot. Eight is enough to keep a producer and a consumer both busy.
constexpr usize PIPE_SLOTS = 8;

using Pipe = Channel<String, PIPE_SLOTS>;

Stream pipe_sink(Pipe &p);

Source pipe_source(Pipe &p);

// A source that is already at end of input. What a stage of a pipeline gets
// when nothing writes to it — a background job's stdin, and the read end of a
// pipe whose only writer went into a child.
Source null_source();

// An open file behind a Stream or a Source. The offset lives here because the
// VFS is not seekable: nothing below it holds per-descriptor state. That is a
// precondition, not just an observation — descriptors share a backend handle.
//
// Both ends are synchronous — that is the whole point of Concept.md §5.2's
// sync access handles — so neither ever parks, and `park` stays null.
struct FileIo {
    i32 fd  = -1;
    u64 off = 0;

    FileIo() = default;

    explicit FileIo(i32 d, u64 at = 0) : fd(d), off(at) {}

    FileIo(const FileIo &)            = delete;
    FileIo &operator=(const FileIo &) = delete;

    ~FileIo() { reset(); }

    void reset()
    {
        if (fd >= 0)
            vfs_close(fd);
        fd  = -1;
        off = 0;
    }
};

Stream file_sink(FileIo &f);

Source file_source(FileIo &f);

// Opens `path` for reading, positioned at the start. The caller keeps the
// FileIo alive for as long as it reads from the Source.
Task<Result<void>> file_open_read(Str path, FileIo &out);

// The whole of a file, for the two callers that want the bytes rather than a
// stream: the image `exec` is about to run, and the motd init prints. The
// process-side twin is read_file in src/proc/io.h.
Task<Result<String>> read_file(Str path);

// Writes all of `s`, retrying the stray wake that leaves a write unfinished.
Task<Result<void>> write_all(Stream out, Str s);

// Splits a source into lines. A line may span any number of chunks, and a
// final fragment with no newline is a line.
//
// The kernel has no reader of its own left — every program is a binary — so
// this is the reference the process-side twin in src/proc/io.h mirrors, and
// test_io is what keeps it honest.
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

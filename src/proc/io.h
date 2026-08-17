// What a process's stdio looks like from inside: syscalls, and the few
// helpers a program would otherwise write again. It mirrors src/user/io.h so a
// program ported from the registry to a binary keeps its text and its exit
// codes; it is much shorter, because everything here is asynchronous and none
// of the Stream/Source machinery is needed to bridge a synchronous read.
#pragma once

#include "kernel/span.h"
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

// This process's own working directory, which every relative path above
// resolves against. Inherited from whoever spawned it — the shell, for a
// command typed at the prompt — and moved by nobody else. Both report the
// resulting absolute path, so a program that has just moved knows where to.
Task<Result<String>> cwd_get();

Task<Result<String>> cwd_set(Str path);

// A pipe, both ends in this process's table. Either may be moved into a child,
// which is what closes this side of it and therefore what lets the other side
// see an end of input.
struct Piped {
    i32 r = -1;
    i32 w = -1;
};

Task<Result<Piped>> make_pipe();

// What a child is entered with. 0, 1 and 2 mean "the stream I was given"; a
// descriptor from SYS_FD_MIN up is *moved* out of this process's table, which
// is what closes a pipe's write end and therefore what gives the reader an end
// of input. It must not be used after the spawn.
struct ChildIo {
    u32 in  = SYS_STDIN;
    u32 out = SYS_STDOUT;
    u32 err = SYS_STDERR;
};

Task<Result<u32>> spawn(Args v, ChildIo io = {});

// The child that stopped, and what it reported. Waiting on SYS_WAIT_ANY takes
// whichever finishes first.
struct Exited {
    u32 pid    = 0;
    i32 status = 0;
};

Task<Result<Exited>> wait_child(u32 pid = SYS_WAIT_ANY);

Task<Result<void>> kill_child(u32 pid);

// Puts a child of this process in front of the console, so that ^C reaches it
// rather than this process — which is what a shell does before it waits. Zero
// takes the console back, and then ^C arrives as an ordinary key instead.
// Err(Perm) unless this process has the terminal already: it holds the raw
// keys, or it is itself what is in front.
Task<Result<void>> set_fg(u32 pid);

// ------------------------------------------------------------- the terminal
//
// The half of it that needs no grid. A program that paints cells wants
// proc/screen.h instead; what is here is what a *prompt* needs — text through
// stdout, which wraps and scrolls, and a cursor to put back where it was.

// The geometry, which every terminal reply carries so that a resize needs no
// event to subscribe to.
struct Geometry {
    u32 cols = 0;
    u32 rows = 0;
};

// Whether a descriptor is the terminal, and how big it is. `at` is zero unless
// `console` is true. The only way to tell a terminal from a pipe: the grid is
// cells (§2.3), so there is no escape sequence to ask with.
struct TtyInfo {
    bool console = false;
    Geometry at;
};

Task<Result<TtyInfo>> tty_of(u32 fd);

// Takes the raw keys, or gives them back. A shell gives them back before it
// runs a foreground child, so the child can claim them in its turn; a second
// claimant while somebody holds them is Err(Perm).
Task<Result<Geometry>> keys_claim(bool take);

// The alternate screen, the same way.
Task<Result<Geometry>> screen_claim(bool take);

// The next key. No control characters exist (Concept.md §3.5): ^C is 'c' with
// the control modifier, and with nothing in front it arrives here.
struct KeyPress {
    u32 code = 0;
    u32 mods = 0;
    Geometry at;
};

Task<Result<KeyPress>> key_read();

// Where the cursor is on the *scrolling* screen. Writing moves it and nothing
// counts the scrolls, so a line editor writes and then asks where that landed.
struct CursorAt {
    u32 x   = 0;
    u32 y   = 0;
    bool on = false;
    Geometry at;
};

Task<Result<CursorAt>> cursor_get();

Task<Result<CursorAt>> cursor_set(u32 x, u32 y, bool on);

// One run of a repaint: the colour it paints in, and the bytes. SYS_STYLE_KEEP
// leaves the sticky style alone; no text sets the colour and paints nothing.
struct StyledRun {
    u32 style = SYS_STYLE_KEEP;
    Str text;
};

// A repaint, in one call: the cursor to the anchor (x, y), a run per colour,
// then the cursor left `cur` cells past the anchor. `scrolled` is how far the
// anchor row went up while the write was happening, which is the only thing a
// caller could not work out for itself.
struct Painted {
    CursorAt cursor;
    u32 scrolled = 0;
};

// `flags` is SYS_ECHO_SHOW, FRESH and END of sysabi.h.
Task<Result<Painted>> cursor_echo(u32 x, u32 y, u32 cur, u32 flags, Span<const StyledRun> runs);

// The colours the next write paints with — COLOR_* and ATTR_* of screen.h. The
// grid is cells, so a colour is not in the bytes; and it is sticky, so a
// program that colours something puts the default back after it.
Task<Result<void>> style_set(u8 fg, u8 bg, u8 attrs);

// What `df` reports (Concept.md §5.3). `known` is false when the host would
// not say, which is not the same as a quota of zero.
struct StorageInfo {
    u64 quota      = 0;
    u64 usage      = 0;
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
// One is open at a time: each is opened when the read reaches it and closed
// before the next. `who` names this program in the diagnostic a failed open
// prints, and is unused when no path was named.
struct Input {
    Input(Args paths, u32 fallback, Str who = {})
        : paths_(paths), who_(who), fd_(fallback), own_(paths.size() > 0)
    {
    }

    Input(const Input &)            = delete;
    Input &operator=(const Input &) = delete;

    // The next chunk of the concatenation, or Err(Closed) at the end of it. A
    // file that will not open is reported here, on stderr, and comes back as
    // its own error — so a caller's Cancelled-is-130 mapping still holds.
    Task<Result<String>> read();

private:
    Args paths_;
    Str who_;
    usize at_ = 0;
    i32 cur_  = -1; // the file at_ names, once opened
    u32 fd_;        // stdin, when no path was named
    bool own_;
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

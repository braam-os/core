// The kernel↔process wire (Concept.md §4.3), spoken by the dispatcher in
// src/user/syscall.cpp and by the process runtime in src/proc/. It sits beside
// host.h for the same reason: an ABI belongs in one file that both ends
// include, so neither can drift alone.
//
// A process's imports are `sys` and `sys_async`, bound by the host to one pid.
// The kernel answers them through two exports of the same names, which take
// the pid the host injects — which is why a process cannot name another.
#pragma once

#include "str.h"
#include "types.h"

// The wasm custom section every process binary carries, and the whole of what
// `exec` needs before it can instantiate one: how much memory to hand it.
// Five u32s so the parser needs no alignment care.
struct ProcMeta {
    u32 magic;
    u32 abi;
    u32 flags;
    u32 initial_pages;
    u32 max_pages;
};

constexpr Str PROC_SECTION   = "braam";
constexpr u32 PROC_MAGIC     = 0x6d617262; // "bram"
constexpr u32 PROC_ABI       = 12;
constexpr u32 PROC_PAGE      = 65536;
constexpr u32 PROC_MAX_PAGES = 256; // 16 MB, the ceiling the kernel imposes

// The other thing `exec` will instantiate: a text file whose first line names an
// interpreter (Concept.md §4.3). The interpreter is absolute and the rest of the
// line is one argument, unsplit. `interp` and `arg` view `image`, and are
// written only on true. Here rather than in exec.cpp because `test -x` gives the
// same answer and cannot reach the kernel's trees; nothing here is on the wire.
constexpr usize PROC_SHEBANG_MAX = 128; // the first line must end inside this

inline bool exec_shebang(Str image, Str &interp, Str &arg)
{
    if (!image.starts_with("#!"))
        return false;

    // Space and tab alone; a newline ends the line rather than separating words.
    auto blank = [](char c) { return c == ' ' || c == '\t'; };

    // A line running past the cap is not a first line.
    usize cap = image.size() < PROC_SHEBANG_MAX ? image.size() : PROC_SHEBANG_MAX;
    usize eol = 2;
    while (eol < cap && image[eol] != '\n')
        eol++;
    if (eol == cap && cap < image.size())
        return false;

    Str line = image.substr(2, eol - 2);
    usize at = 0, end = line.size();
    while (end > at && (blank(line[end - 1]) || line[end - 1] == '\r'))
        end--;
    while (at < end && blank(line[at]))
        at++;
    if (at == end || line[at] != '/')
        return false;

    usize name = at;
    while (at < end && !blank(line[at]))
        at++;
    interp = line.substr(name, at - name);
    while (at < end && blank(line[at]))
        at++;
    arg = line.substr(at, end - at);
    return true;
}

// What a spawn request's `flags` word carries: the two page counts the host
// needs before it can make a Memory. One word because the record has no second
// scalar left — `aux` is the pid, and nothing else may ride on that.
static_assert(u32(PROC_MAX_PAGES) < 65536, "the page counts no longer fit in one word");

inline u32 proc_pack(const ProcMeta &m)
{
    return m.initial_pages | (m.max_pages << 16);
}

inline u32 proc_initial(u32 flags)
{
    return flags & 0xffff;
}

inline u32 proc_max(u32 flags)
{
    return flags >> 16;
}

// Syscalls. The synchronous half answers inside the export and never parks;
// the asynchronous half records a request the process's proxy task performs,
// and its reply reaches the process through _resume.
//
// The synchronous half is closed: all four are answered inside the process's
// own worker, with no kernel to ask (Concept.md §4.3), so an operation that
// needs the kernel has to be asynchronous whatever it costs.
enum class Sys : u32 {
    // sys(op, a0, a1, a2) -> i32
    Exit = 1, // a0 = exit status
    GetPid,   // -> pid
    Now,      // -> milliseconds since boot
    Stage,    // a0 = bytes the host is about to copy in; -> kernel address, or 0

    // sys_async(op, token, ptr, len). The reply payload is an i32 status
    // followed by any data: _resume's signature has no room for both. The op
    // word's upper bits are the operation's argument — a descriptor, or one
    // small immediate — so a write hands over the bytes themselves rather than
    // a copy with a header glued on the front.
    //
    // Descriptors and the names they come from. A number one process holds
    // means nothing in another: it indexes that process's own table.
    Write = 16, // arg = fd;             payload = the bytes;  status = bytes written
    Read,       // arg = fd;                                   data = the chunk, empty at end
    Open,       // arg = SYS_O_* flags;  payload = the path;   status = the fd
    Close,      // arg = fd
    Stat,       // payload = the path;   data = u32 kind, u64 size, u64 mtime
    List,       // payload = the path;   data = u32 count, then that many entries
    MkDir,      // payload = the path
    Remove,     // arg bit 0 = recursive; payload = the path
    Touch,      // payload = the path;   an existing file's mtime moved to now

    // The working directory the five above resolve against, which is this
    // process's own and nobody else's — inherited from whoever spawned it, the
    // shell for a top-level command. It sits in this block rather than with the
    // process family because it is the state those operations read, and a
    // program that never spawns still moves it.
    Chdir, // arg bit 0 = set, else just report; payload = the path; data = the cwd

    // A second descriptor for one open thing, which is the only way to say
    // `2>&1` and the only way a shell's `exec >file` survives a Spawn — that
    // moves a descriptor rather than copying it. One handle behind both, so a
    // file's offset is shared and the two streams interleave.
    Dup, // arg = fd;  status = the new fd

    // Time. The timer queue is the kernel's, and Sys::Now is monotonic and
    // says nothing about a day, so both of these have to be asked for.
    Sleep = 32, // payload = u32 milliseconds

    // Host services. Anything the kernel already publishes as text under /proc
    // is read with Open and Read instead — `mount` and `pwd` need no operation
    // of their own, and a syscall nothing calls is an ABI nothing tests.
    // Anything that is a stream of bytes comes back as a descriptor, so Read,
    // Write and Close serve it and no operation is duplicated: a fetched body
    // is read like a file, a socket is written like one, and a killed process
    // drops all of them when its handle table goes.
    Clock = 48, // data = u64 epoch_ms, i32 tz_min
    Storage,    // data = u64 quota, u64 usage, u32 flags
    Fetch,      // payload = u32 url_len, the url, the spec
                //   status = the body's fd; data = u32 http status, the headers
    WsOpen,     // payload = the url; status = the socket's fd
    ClipRead,   // arg bit 0 = wait for a paste instead;  data = the text
    ClipWrite,  // payload = the text
    Pick,       // status = the set's fd;  data = u32 count, then a name each
    PickOpen,   // arg = the set's fd; payload = u32 index; status = the file's fd
    Fexport,    // payload = u32 name_len, the name, the bytes

    // The terminal. Cells, never a byte stream (§2.3), so a full-screen
    // program paints a grid of its own and blits the part of it that changed.
    // Both claims are held by the *kernel*, on the process's record: a killed
    // process runs no destructor, and the screen and the keyboard have to come
    // back anyway.
    KeyClaim = 64, // arg bit 0 = take, else give back;  data = u32 cols, u32 rows
    KeyRead,       // data = u32 code, u32 mods, u32 cols, u32 rows
    ScreenEnter,   // arg bit 0 = the alternate screen, else back; data = cols, rows
    ScreenBlit,    // payload = u32 x, y, w, h, cursor_x, cursor_y, cursor_on, then w*h Cells
    ScreenClear,   // blank the shell's screen and home its cursor

    // The cursor of the *scrolling* screen, which is the one a prompt lives in.
    // Get and set in one operation, like KeyClaim above and Chdir: a line editor
    // writes with Write — which wraps and scrolls — and then asks where that
    // landed, because nothing counts scrolls. A set is refused while another
    // process holds the alternate screen, for ScreenBlit's reason.
    Cursor, // arg bit 0 = set;  payload = u32 x, y, on
            //   data = u32 x, y, on, cols, rows

    // The colours the next Write paints with. Sticky grid state, as it is for
    // the kernel's own writers, and refused while another process holds the
    // alternate screen — Cursor's rule, for Cursor's reason.
    Style, // arg = fg | bg << 8 | attrs << 16 (sys_style_pack)

    // A line editor's whole repaint, in one operation: the anchor, a run per
    // colour, the bytes, and where to leave the cursor — carried up by whatever
    // the write scrolled under it, which is what `scrolled` reports and what
    // Cursor was being asked for. Cursor's rules, for Cursor's reason.
    //
    // A run is a Style and a Write, FRESH is a Cursor get and a newline, and
    // END is not calling Cursor afterwards: nothing four existing operations
    // could not already do, at one round trip instead of seven.
    Echo, // arg = SYS_ECHO_SHOW | SYS_ECHO_FRESH | SYS_ECHO_END
          //   payload = u32 x, y, cur, runs, then runs * (u32 style, u32 len),
          //             then the runs' bytes end to end
          //   data    = u32 x, y, on, cols, rows, scrolled

    // Whether a descriptor is the terminal, and how big it is. Only the kernel
    // knows: the grid is cells (§2.3), so there is no escape sequence to ask
    // with, and a COLUMNS in the environment is one a resize has made wrong.
    Tty, // arg = fd;  data = u32 flags, u32 cols, u32 rows

    // Processes. A program that supervises another one cannot be a shell
    // builtin — the builtins are the six things no syscall could serve — so
    // this is what makes `timeout` and `watch` writable at all.
    //
    // A descriptor named in a Spawn is *moved*: the parent's slot is closed and
    // the child owns it. That is what closes a pipe's write end and therefore
    // what produces the reader's end of input, and it keeps one process holding
    // one end, which is what Channel's single-sender rule needs (channel.h).
    Pipe = 80, // data = u32 read fd, u32 write fd
    Spawn,     // arg bit 0 = an env blob follows, else the child inherits mine
               //   payload = u32 fd0, fd1, fd2, the argv blob, then the env blob
               //   status = the child's pid
               //   an fd below SYS_FD_MIN shares the stream this process was given
    Wait,      // arg = a pid, or SYS_WAIT_ANY;  status = the child's status; data = u32 pid
    Kill,      // arg = the pid, which must be a child of the caller

    // Which process is in front of the console, and therefore what ^C reaches.
    // With nobody in front, ^C is delivered to whoever holds the raw keys as an
    // ordinary key — which is how a line editor abandons the line being typed
    // rather than being cancelled by it.
    //
    // The caller must have the terminal already: it holds the raw-key claim, or
    // it is itself what is in front. That is what stops a background program
    // taking ^C away from the shell.
    Fg, // arg = a child's pid, or 0 to take the console back
};

// The header ScreenBlit's payload begins with, in u32s.
constexpr usize SYS_BLIT_HEAD = 7;

// The three descriptor words Sys::Spawn's payload begins with, in u32s.
constexpr usize SYS_SPAWN_HEAD = 3;

// Sys::Spawn's op-word argument: an env blob follows the argv one. Without it
// the child inherits the caller's environment.
constexpr u32 SYS_SPAWN_ENV = 1;

// The most an environment may be, and therefore what a descendant carries.
constexpr u32 SYS_ENV_MAX = 8192;

// The anchor, cursor offset and run count Sys::Echo's payload begins with, and
// the style and length of each run header after them, in u32s. Every header
// precedes every byte, so the whole shape is checkable in one pass.
constexpr usize SYS_ECHO_HEAD = 4;
constexpr usize SYS_ECHO_RUN  = 2;

// The most runs one Echo may carry. A prompt is four, and an unbounded count is
// a loop a hostile binary chooses.
constexpr u32 SYS_ECHO_RUNS_MAX = 8;

// Sys::Echo's op-word argument. FRESH anchors wherever the cursor is, on a row
// of its own; END leaves the cursor where the write ended rather than `cur`
// cells past the anchor.
constexpr u32 SYS_ECHO_SHOW  = 1;
constexpr u32 SYS_ECHO_FRESH = 2;
constexpr u32 SYS_ECHO_END   = 4;

// Sys::Wait's "whichever finishes first". Zero is never a pid.
constexpr u32 SYS_WAIT_ANY = 0;

// The largest pid there is. Above it are the scheduler's anonymous jobs, which
// this wire cannot name.
constexpr u32 SYS_PID_MAX = 999999;

// The op word's argument is 24 bits, and a pid must survive it whole.
static_assert(SYS_PID_MAX < (1u << 24), "a pid must fit the op word's argument");

// How many children a process may have at once, and how deep a chain of them
// may go. Every one is an instance with a memory cap of its own, so without a
// bound the first fork bomb takes the page with it. Sixteen either way because
// a shell is a process now: a pipeline is up to eight stages and a background
// job it has not reaped yet still holds an entry, and a nested `sh` spends a
// level of depth per prompt.
constexpr usize SYS_CHILD_MAX = 16;
constexpr u32 SYS_PROC_DEPTH  = 16;

// The most a blit may carry, which is the largest grid there can be. Sys::Stage
// is capped at the same number: a process asks the host for room before it
// sends, and an uncapped ask is an allocation a hostile binary chooses.
constexpr u32 SYS_STAGE_MAX = 1u << 20;

// One entry of Sys::List's reply: u32 kind, u64 size, u64 mtime, u32 name_len,
// the name. `mtime` is milliseconds since the epoch, 0 when the filesystem
// keeps none. Restated here rather than shared with fs.h, for the reason
// SYS_O_* is: a process cannot see the VFS, and the numbers a binary compiled
// today speaks must not move because the filesystem's did.
constexpr u32 SYS_KIND_FILE = 0;
constexpr u32 SYS_KIND_DIR  = 1;

// The flags word Sys::Storage answers with (Concept.md §5.3).
enum : u32 {
    SYS_STORE_OPFS      = 1,
    SYS_STORE_SYNC      = 2,
    SYS_STORE_PERSISTED = 4,
    SYS_STORE_KNOWN     = 8, // the host answered at all
};

// The flags word Sys::Tty answers with.
enum : u32 {
    SYS_TTY_CONSOLE = 1, // this descriptor is the cell grid
};

inline u32 sys_op(Sys op, u32 arg = 0)
{
    return u32(op) | (arg << 8);
}

inline Sys sys_op_code(u32 op)
{
    return Sys(op & 0xff);
}

inline u32 sys_op_arg(u32 op)
{
    return op >> 8;
}

// The same field, named for what it holds at the descriptor operations.
inline u32 sys_op_fd(u32 op)
{
    return sys_op_arg(op);
}

// Sys::Style's argument: two palette indices and the ATTR_* bits of screen.h,
// which fit the op word's 24 with a byte to spare — so the operation carries no
// payload at all and needs no staging.
inline u32 sys_style_pack(u8 fg, u8 bg, u8 attrs)
{
    return u32(fg) | (u32(bg) << 8) | (u32(attrs) << 16);
}

inline u8 sys_style_fg(u32 arg)
{
    return u8(arg);
}

inline u8 sys_style_bg(u32 arg)
{
    return u8(arg >> 8);
}

inline u8 sys_style_attrs(u32 arg)
{
    return u8(arg >> 16);
}

// The style of a Sys::Echo run that names no colour: the sticky one stands, as
// it does for a Write. Outside sys_style_pack's range, whose top byte is zero.
constexpr u32 SYS_STYLE_KEEP = 0xffffffff;
static_assert(SYS_STYLE_KEEP > 0x00ffffff, "SYS_STYLE_KEEP collides with sys_style_pack");

// The three stdio descriptors are the stage's Stdio; anything above indexes
// the open-file table the process record owns.
constexpr u32 SYS_STDIN  = 0;
constexpr u32 SYS_STDOUT = 1;
constexpr u32 SYS_STDERR = 2;
constexpr u32 SYS_FD_MIN = 3;

// Open flags, restated here rather than shared with src/fs/fs.h: a process
// cannot see the VFS, and the numbers a binary compiled today speaks must not
// move because the filesystem's did. exec maps them.
constexpr u32 SYS_O_READ   = 1;
constexpr u32 SYS_O_WRITE  = 2;
constexpr u32 SYS_O_CREATE = 4;
constexpr u32 SYS_O_TRUNC  = 8;
constexpr u32 SYS_O_APPEND = 16;

// One read, and the most a write should hand over at once: FS_BLOCK is the
// allocator's top size class on both sides of the wire, and one byte more
// costs a whole 64 KiB span (Concept.md §8.2).
constexpr u32 SYS_CHUNK = 512;

// `test -x` decides from one chunk (src/cmd/sh/condrun.cpp).
static_assert(PROC_SHEBANG_MAX <= SYS_CHUNK, "test -x could no longer see a whole #! line");

// The outcome of one _start or _resume, as the host reports it.
enum class ProcStep : u32 {
    Exited = 0,
    Suspended,
    Trapped,
};

// ------------------------------------------------------------- byte helpers

inline u32 sys_get_u32(const u8 *p)
{
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

inline void sys_put_u32(u8 *p, u32 v)
{
    p[0] = u8(v);
    p[1] = u8(v >> 8);
    p[2] = u8(v >> 16);
    p[3] = u8(v >> 24);
}

// ------------------------------------------------------- the argv and the env

// argv is one blob rather than a pointer array, because it crosses an address
// space: u32 argc, then u32 length and bytes per word. The host copies it into
// the process with _alloc and passes it to _start.
//
// The environment is a word list in the same encoding, its words NAME=value.
// The two sit back to back — in _start's payload, and after Sys::Spawn's three
// descriptor words — and argv_bytes finds the join.

inline usize argv_size(const Str *v, usize n)
{
    usize total = 4;
    for (usize i = 0; i < n; i++)
        total += 4 + v[i].size();
    return total;
}

inline void argv_encode(const Str *v, usize n, u8 *out)
{
    sys_put_u32(out, u32(n));
    usize at = 4;
    for (usize i = 0; i < n; i++) {
        sys_put_u32(out + at, u32(v[i].size()));
        at += 4;
        for (usize k = 0; k < v[i].size(); k++)
            out[at + k] = u8(v[i][k]);
        at += v[i].size();
    }
}

inline usize argv_count(const u8 *p, usize len)
{
    return len < 4 ? 0 : sys_get_u32(p);
}

// The bytes one blob occupies, so a second may follow it. 0 is malformed: the
// smallest well-formed blob is the four of an empty word list.
inline usize argv_bytes(const u8 *p, usize len)
{
    if (len < 4)
        return 0;
    usize n  = sys_get_u32(p);
    usize at = 4;
    for (usize k = 0; k < n; k++) {
        if (at + 4 > len)
            return 0;
        usize size = sys_get_u32(p + at);
        at += 4;
        if (at + size > len)
            return 0;
        at += size;
    }
    return at;
}

// The i'th word, or an empty Str if the blob is short — a truncated blob is a
// broken host, and reading past it would be worse than losing an argument.
inline Str argv_at(const u8 *p, usize len, usize i)
{
    usize n = argv_count(p, len);
    if (i >= n)
        return Str();
    usize at = 4;
    for (usize k = 0; k < n; k++) {
        if (at + 4 > len)
            return Str();
        usize size = sys_get_u32(p + at);
        at += 4;
        if (at + size > len)
            return Str();
        if (k == i)
            return Str(reinterpret_cast<const char *>(p + at), size);
        at += size;
    }
    return Str();
}

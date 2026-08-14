// The kernel↔process wire (Concept.md §4.3), spoken by the dispatcher in
// src/user/exec.cpp and by the process runtime in src/proc/. It sits beside
// host.h for the same reason: an ABI belongs in one file that both ends
// include, so neither can drift alone.
//
// A process's imports are `sys` and `sys_async`, bound by the host to one pid.
// The kernel answers them through two exports of the same names, which take
// the pid the host injects — which is why a process cannot name another.
#pragma once

#include "str.h"
#include "types.h"

// Isolation tiers (Concept.md §4). Tier 1 is an in-kernel coroutine and never
// appears in a binary; tier 3 is a worker of its own, and a binary asking for
// it runs at tier 2 where the host cannot make one.
enum class Tier : u8 {
    Applet = 1,
    Instance,
    Worker,
};

// The wasm custom section every process binary carries, and the whole of what
// `exec` needs before it can instantiate one: which tier to run it at, and how
// much memory to hand it. Eight u32s so the parser needs no alignment care.
struct ProcMeta {
    u32 magic;
    u32 abi;
    u32 tier;
    u32 flags;
    u32 initial_pages;
    u32 max_pages;
};

constexpr Str PROC_SECTION   = "braam";
constexpr u32 PROC_MAGIC     = 0x6d617262; // "bram"
constexpr u32 PROC_ABI       = 1;
constexpr u32 PROC_PAGE      = 65536;
constexpr u32 PROC_MAX_PAGES = 256; // 16 MB, the ceiling the kernel imposes

// What a spawn request's `flags` word carries: the two page counts the host
// needs before it can make a Memory, and the tier that says where to put the
// instance. One word because the record has no second scalar left — `aux` is
// the pid, and nothing else may ride on that.
static_assert(u32(PROC_MAX_PAGES) < 4096, "the page counts no longer fit beside the tier");

inline u32 proc_pack(const ProcMeta &m, Tier tier)
{
    return m.initial_pages | (m.max_pages << 16) | (u32(tier) << 28);
}

inline u32 proc_initial(u32 flags)
{
    return flags & 0xffff;
}

inline u32 proc_max(u32 flags)
{
    return (flags >> 16) & 0xfff;
}

inline Tier proc_tier(u32 flags)
{
    return Tier(flags >> 28);
}

// Syscalls. The synchronous half answers inside the export and never parks;
// the asynchronous half records a request the process's proxy task performs,
// and its reply reaches the process through _resume.
enum class Sys : u32 {
    // sys(op, a0, a1, a2) -> i32
    Exit = 1, // a0 = exit status
    GetPid,   // -> pid
    Now,      // -> milliseconds since boot
    Stage,    // a0 = bytes the host is about to copy in; -> kernel address, or 0

    // sys_async(op, token, ptr, len). The reply payload is an i32 status
    // followed by any data: _resume's signature has no room for both. The op
    // word carries the descriptor in its upper bits, so a write hands over the
    // bytes themselves rather than a copy with a header glued on the front.
    Write = 16, // fd in the op; payload = the bytes;  status = bytes written
    Read,       // fd in the op;                       data = the chunk, empty at end
    Open,       // payload = u32 flags, then the path; status = the fd
    Close,      // fd in the op
};

inline u32 sys_op(Sys op, u32 fd = 0)
{
    return u32(op) | (fd << 8);
}

inline Sys sys_op_code(u32 op)
{
    return Sys(op & 0xff);
}

inline u32 sys_op_fd(u32 op)
{
    return op >> 8;
}

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

// ---------------------------------------------------------------- the argv

// argv is one blob rather than a pointer array, because it crosses an address
// space: u32 argc, then u32 length and bytes per word. The host copies it into
// the process with _alloc and passes it to _start.

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

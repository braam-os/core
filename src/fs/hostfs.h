// The kernel's side of the storage ABI (Concept.md §5.2, §5.3).
//
// An asynchronous operation is a record in linear memory whose address goes to
// the host; the reply comes back through wake(), as §2.2 requires. The record
// is not in the awaiting coroutine's frame, because a cancelled await destroys
// that frame while the host still holds the address — so a request outlives its
// awaiter, joins an orphan list, and is freed when its reply finally lands.
#pragma once

#include "fs.h"
#include "kernel/sched.h"
#include "kernel/string.h"

enum class FsOp : u32 {
    Info = 1, // the StorageBackend, into buf
    Bundle,   // the boot archive, into buf
    Open,     // path, flags -> result = handle
    Stat,     // path -> flags = NodeKind, result = size
    List,     // path -> packed entries, into buf
    Mkdir,    // path
    Remove,   // path, flags bit 0 = recursive
};

// host_fs_sync's operations. All of them act on an already-open handle.
enum class FsSyncOp : u32 {
    Read = 1, // -> bytes read
    Write,    // -> bytes written
    Size,     // -> size, which therefore cannot exceed 2 GiB
    Truncate,
    Flush,
    Close,
};

// The record the host reads and writes. web/fs.js decodes this layout field by
// field, so the two must be changed together.
struct FsRequest {
    u32 op;
    u32 token;
    u32 path_ptr, path_len;
    u32 flags;
    u32 buf_ptr, buf_cap;
    i32 status; // 0, or the negated Error the host chose
    u32 result_lo, result_hi;
    u32 buf_len; // bytes written into buf, or the capacity the reply needs
};

// One outstanding request: the header the host sees, plus the buffers it points
// at, which the kernel owns for exactly as long as the host may touch them.
struct FsReq {
    FsRequest h{};
    String path;
    String buf;
    bool done   = false;
    bool orphan = false;

    u64 result() const { return u64(h.result_lo) | (u64(h.result_hi) << 32); }
};

// Issues one operation and suspends until its reply. Used as an awaiter
// directly, so that setting the buffer up and reading the answer out happen
// either side of a single `co_await` with no intervening coroutine frame.
struct FsCall {
    FsCall(FsOp op, Str path, u32 flags);

    FsCall(const FsCall &)            = delete;
    FsCall &operator=(const FsCall &) = delete;

    ~FsCall();

    // False when the record itself could not be allocated.
    bool ok() const { return r_ != nullptr; }

    FsReq &req() { return *r_; }

    // Gives the host somewhere to write a variable-sized reply.
    bool reserve(usize n);

    bool await_ready() const noexcept { return false; }

    template <class P>
    bool await_suspend(std::coroutine_handle<P> h)
    {
        w_.h      = h;
        w_.cancel = h.promise().cancel;
        if (!r_ || (w_.cancel && w_.cancel->cancelled)) {
            w_.cancelled = true;
            return false;
        }
        // A fresh token per call: a record may be awaited twice, when the
        // first reply only reported how much room the second one needs.
        w_.token = sched_token();
        if (!sched_wait_token(&w_)) {
            w_.failed = true;
            return false;
        }
        issue();
        issued_ = true;
        return true;
    }

    Result<void> await_resume() const;

private:
    void issue();

    FsReq *r_    = nullptr;
    bool issued_ = false; // the host has the record's address
    Waiter w_;
};

// A wasm32 pointer as the integer the host indexes memory with.
u32 host_addr(const void *p);

// Called from wake() for a token nothing is waiting on: if it belongs to an
// abandoned request, that record can finally be freed.
void fs_orphan_reply(u32 token);

// Outstanding abandoned requests, for the leak checks in the tests.
usize fs_orphans();

// Capabilities and usage, straight from the host (Concept.md §5.3).
Task<Result<StorageBackend>> storage_info();

// The boot archive BundleFs parses, or Err(NotFound) when the host has none.
Task<Result<String>> storage_bundle();

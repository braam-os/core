// One asynchronous request to the host, for every interface that has one
// (Concept.md §2.2, §3.4).
//
// The request is a record in linear memory whose address goes to the host; the
// reply comes back through wake(). The record is not in the awaiting
// coroutine's frame, because a cancelled await destroys that frame while the
// host still holds the address — so a request outlives its awaiter, joins an
// orphan list, and is freed when its reply finally lands.
#pragma once

#include "jsref.h"
#include "result.h"
#include "sched.h"
#include "str.h"
#include "string.h"
#include "traits.h"
#include "types.h"

// Which import carries the request. One per calling convention, not one per
// operation, which is the shape §4.3 fixes for the process ABI.
enum class HostIface : u32 {
    Fs = 1,
    Svc,
};

// The record the host reads and writes. web/abi.js decodes this layout field
// by field, so the two must be changed together.
struct HostRequest {
    u32 op;
    u32 token;
    u32 arg_ptr, arg_len; // the inline string argument: a path, a URL, a name
    u32 flags;
    u32 buf_ptr, buf_cap;
    i32 status; // 0, or the negated Error the host chose
    u32 result_lo, result_hi;
    u32 buf_len; // bytes written into buf, or the capacity the reply needs
    u32 ref;     // the JsRef slot the operation acts on, or deposits into
    u32 aux;     // a second scalar argument; the pid, for the process ops (§4.3)
};

// One outstanding request: the header the host sees, plus the buffers it points
// at, which the kernel owns for exactly as long as the host may touch them.
struct HostReq {
    HostRequest h{};
    String arg;
    String buf;
    JsRef ref; // the slot a reply deposits into, held here so an orphan frees it
    bool done   = false;
    bool orphan = false;

    u64 result() const { return u64(h.result_lo) | (u64(h.result_hi) << 32); }
};

// Issues one operation and suspends until its reply. Used as an awaiter
// directly, so that setting the buffer up and reading the answer out happen
// either side of a single `co_await` with no intervening coroutine frame.
struct HostCall {
    HostCall(HostIface iface, u32 op, Str arg, u32 flags);

    HostCall(const HostCall &)            = delete;
    HostCall &operator=(const HostCall &) = delete;

    ~HostCall();

    // False when the record itself could not be allocated.
    bool ok() const { return r_ != nullptr; }

    HostReq &req() { return *r_; }

    // Gives the host somewhere to write a variable-sized reply.
    bool reserve(usize n);

    // Fills the buffer the host is to read, for a request that carries bytes.
    bool put(Str bytes);

    // The same, taking ownership: a process image is tens of kilobytes and the
    // caller has one already (Concept.md §4.4).
    bool put(String &&bytes);

    // A second scalar the host reads out of the record. The process ops put
    // the pid here, since op/flags are spoken for.
    void set_aux(u32 v) { r_->h.aux = v; }

    // Borrows an object the host already holds: the request names the slot but
    // does not own it.
    void set_ref(JsSlot slot) { r_->h.ref = slot; }

    // Claims a slot for the host to deposit an object in. The record owns it
    // until await_resume hands it over, so a cancelled request frees it.
    bool reserve_ref();

    JsRef take_ref() { return move(r_->ref); }

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

    HostReq *r_  = nullptr;
    bool issued_ = false; // the host has the record's address
    HostIface iface_;
    Waiter w_;
};

// A wasm32 pointer as the integer the host indexes memory with.
u32 host_addr(const void *p);

// Called from wake() for a token nothing is waiting on: if it belongs to an
// abandoned request, that record can finally be freed.
void host_orphan_reply(u32 token);

// Outstanding abandoned requests, for the leak checks in the tests.
usize host_orphans();

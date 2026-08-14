// Host services (Concept.md §3.7): fetch, WebSocket, the clipboard, file
// transfer and the wall clock, all over the one asynchronous convention of
// kernel/hostcall.h. None of them is promise-free on the host side, so none of
// them gets a synchronous import — §2.2 still sanctions exactly two.
#pragma once

#include "kernel/hostcall.h"
#include "kernel/task.h"
#include "kernel/traits.h"

enum class SvcOp : u32 {
    Clock = 1, // -> result = epoch ms, flags = 1440 + minutes east of UTC
    Drop,      // ref: the host lets go, closing a socket or cancelling a body
    Fetch,     // arg = url, buf = the request spec -> result = status, buf = headers, ref = body
    Read,      // ref = body -> buf, as much as fits; buf_len 0 is the end
    WsOpen,    // arg = url -> ref = the socket
    WsSend,    // ref, buf = the message
    WsRecv,    // ref -> buf = the next message, or flags = 1 when the peer went away
    ClipRead,  // -> buf
    ClipWrite, // buf
    Pick,      // -> ref = the chosen files, result = how many
    PickName,  // ref, flags = index -> buf
    PickRead,  // ref, flags = index, result = offset -> buf, as much as fits
    Save,      // arg = the file name, buf = the bytes
    ClipWait,  // -> buf, once the user pastes

    // Isolated processes (Concept.md §4.3), in src/svc/proc.h. They are here
    // rather than behind an import of their own because §2.2 asks for one
    // import per calling convention, and this is that convention exactly.
    ProcSpawn, // aux = pid, arg = path, buf = the image, flags = the pages and the tier
    ProcStep,  // aux = pid, buf = the payload for _start or _resume
               //   -> result_lo = ProcStep
    ProcKill,  // req = pid; no record, no reply
};

// A service operation. The record's inline string argument is a URL or a name.
struct SvcCall : HostCall {
    SvcCall(SvcOp op, Str arg, u32 flags) : HostCall(HostIface::Svc, u32(op), arg, flags) {}

    explicit SvcCall(SvcOp op) : SvcCall(op, "", 0) {}
};

// The reply-sized-twice protocol: the first attempt reports the room it needs
// and writes nothing, and the retry is given a buffer that size.
Task<Result<String>> svc_blob(SvcCall &c);

// A chunk of at most FS_BLOCK bytes, empty at the end of the stream.
Task<Result<String>> svc_chunk(SvcCall &c);

// Tells the host to release an object: a socket is closed, a body cancelled.
void svc_drop(JsSlot slot);

// A host object the kernel holds. Freeing the slot is not enough on its own:
// the host keeps its own reference — an open socket has event handlers — so
// letting go has to be said out loud.
struct JsHandle {
    JsHandle() = default;

    explicit JsHandle(JsRef r) : ref_(move(r)) {}

    JsHandle(JsHandle &&)            = default;
    JsHandle &operator=(JsHandle &&) = default;

    ~JsHandle() { svc_drop(ref_.slot()); }

    bool ok() const { return ref_.ok(); }

    JsSlot slot() const { return ref_.slot(); }

private:
    JsRef ref_;
};

struct WallClock {
    u64 epoch_ms = 0;
    i32 tz_min   = 0; // minutes east of UTC, as the browser's locale has it
};

Task<Result<WallClock>> svc_clock();

Task<Result<String>> clip_read();
Task<Result<void>> clip_write(Str text);

// The clipboard the other way round: browsers only disclose it inside a user
// gesture handler, and a command runs long after the keystroke that started it
// has returned. So when clip_read() is refused, this parks until the user
// pastes, which is a gesture no permission is needed for.
Task<Result<String>> clip_wait();

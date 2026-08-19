// The kernel's side of a running process: the record, the descriptors behind
// its numbers, and the calls it is parked on (Concept.md §4.3).
//
// Private to src/user/ — exec.cpp, syscall.cpp and proctab.cpp are the whole of
// it, and nothing outside the directory has any business with these types. They
// are at namespace scope rather than in an anonymous one only because three
// translation units share them; exec.h is the surface anyone else uses.
#pragma once

#include "io.h"
#include "kernel/alloc.h"
#include "kernel/channel.h"
#include "kernel/sched.h"
#include "kernel/string.h"
#include "kernel/sysabi.h"
#include "kernel/task.h"
#include "kernel/vec.h"
#include "prog.h"
#include "svc/net.h"
#include "svc/xfer.h"
#include "tty.h"

// A pipe between two processes: the shell's Pipe on the heap and refcounted,
// because its two ends are two descriptors that close in either order and may
// by then be held by two different processes.
struct ProcPipe {
    u32 refs = 1;
    Pipe ch;
};

void pipe_release(ProcPipe *q);

// One end of one, held as a member so ~Handle stays implicit. Dropping the
// write end is close() — the reader drains what is queued and then reads end of
// input; dropping the read end is hangup(), which stops the writer.
struct PipeEnd {
    ~PipeEnd()
    {
        shut();
        if (q)
            pipe_release(q);
    }

    // Hangs the end up without letting go of the channel, so a Close still
    // gives the other end its end of input while a parked call finishes.
    void shut()
    {
        if (!q)
            return;
        if (writer)
            q->ch.close();
        else
            q->ch.hangup();
    }

    ProcPipe *q = nullptr;
    bool writer = false;
};

// A descriptor, whatever is behind it. The kinds beyond File are the host
// services that hand back a JS object: a fetch body, a socket, a set of picked
// files, and the two ends of a pipe. Making them descriptors is what lets
// `read`, `write` and `close` serve all of them — six operations the ABI does
// not need — and what makes a killed process drop them, since ~Handle releases
// the externref slot and the host object with it, with no code of its own to
// reach.
struct Handle {
    enum class Kind : u8 { File, Body, Socket, PickSet, PickFile, PipeRead, PipeWrite };

    explicit Handle(Kind k) : kind(k) {}

    Handle(const Handle &)            = delete;
    Handle &operator=(const Handle &) = delete;

    // Closes what is behind the descriptor, leaving the block for whoever is
    // still pointing at it. Separate from ~Handle because a Close must reach
    // the host now, while the block and its externref slot wait.
    void shut()
    {
        switch (kind) {
        case Kind::File:
            file.reset();
            break;
        case Kind::Body:
            res.body.drop();
            break;
        case Kind::Socket:
            sock.sock.drop();
            break;
        case Kind::PickSet:
            pick.set.drop();
            break;
        case Kind::PickFile:
            break; // it owns nothing; its set is a descriptor of its own
        case Kind::PipeRead:
        case Kind::PipeWrite:
            pipe.shut();
            break;
        }
    }

    Kind kind;
    bool busy_r = false; // one reader and one writer at a time (HandleBusy)
    bool busy_w = false;
    u32 refs    = 1; // the table's, plus one for each syscall in flight
    u32 fds     = 1; // descriptors naming it: Sys::Dup makes a second

    FileIo file;      // File
    HttpResponse res; // Body
    WebSocket sock;   // Socket
    Picked pick;      // PickSet
    PipeEnd pipe;     // PipeRead, PipeWrite
    u32 set  = 0;     // PickFile: the descriptor of the set it came from
    usize ix = 0;     // PickFile: which of that set's files
    u64 off  = 0;     // Body and PickFile: how far it has been read
};

void handle_release(Handle *h);

// A counted reference for the length of one syscall: a read that parks must not
// have its descriptor freed under it by a Close in another task of the process.
// Null-tolerant, for the set a PickFile reads through.
struct HandleRef {
    explicit HandleRef(Handle *q) : h(q)
    {
        if (h)
            h->refs++;
    }

    HandleRef(const HandleRef &)            = delete;
    HandleRef &operator=(const HandleRef &) = delete;

    ~HandleRef() { handle_release(h); }

    Handle *h;
};

// Arms the one-user-per-direction guard for the length of a syscall, and
// disarms it however the syscall leaves — including a frame destroyed while
// parked. On a pipe end it is the tripwire Channel's rules need: a second
// blocked sender panics and a second suspended receiver is displaced silently
// (channel.h), either of which would be a user program reaching a kernel
// invariant. On the host kinds the reason is weaker but real — svc_blob's
// sized-twice reply is not re-entrant per object, and `off` would advance
// twice — so a second concurrent user is refused rather than raced.
struct HandleBusy {
    explicit HandleBusy(Handle *q, bool w) : h(q), writer(w)
    {
        (writer ? h->busy_w : h->busy_r) = true;
    }

    HandleBusy(const HandleBusy &)            = delete;
    HandleBusy &operator=(const HandleBusy &) = delete;

    ~HandleBusy() { (writer ? h->busy_w : h->busy_r) = false; }

    Handle *h;
    bool writer;
};

// One syscall a process is parked on: what it asked for, the bytes it staged,
// and the token it will be resumed with. A record rather than three fields on
// the process, because a process may have several tasks and therefore several
// calls outstanding at once — each served by a scheduler job of its own, since
// one of them may be a socket read that never completes.
struct Call {
    ~Call() { heap_free(stage); }

    u32 op     = 0;
    u32 len    = 0;
    u32 token  = 0;
    u8 *stage  = nullptr; // where the host copies the payload
    usize cap  = 0;
    u32 server = 0; // the scheduler job performing it
};

// What a finished call hands back to the stepper: which one it was, and the
// reply the process is to be resumed with.
struct Reply {
    u32 token = 0;
    String payload;
};

constexpr usize PROC_REPLIES = 16; // more than PROC_TASKS, so a send never parks

// One child a process started. It outlives the child itself, because an
// uncollected status is the whole point of Sys::Wait; the Wait that reports one
// erases it, so no child is reaped twice and a process that never waits is
// bounded by SYS_CHILD_MAX rather than by how many it started.
struct Child {
    u32 pid      = 0;
    bool running = true;
    i32 status   = 0;
    u32 wait     = 0; // the token a Wait on this pid is parked on
};

// The kernel's side of one running process. The instance itself is the host's
// — the kernel holds what only the kernel can hold: the stdio the stage was
// given, the descriptors the process opened, and the calls it is waiting on.
struct Proc {
    Proc(u32 p, Stdio s) : pid(p), io(s) { io.retain(); }

    ~Proc()
    {
        // A child never collected still holds its pid reserved.
        for (const Child &ch : children)
            sched_pid_drop(ch.pid);
        heap_delete(alt); // restores the screen
        heap_delete(keys);
        for (Call *c : calls)
            heap_delete(c);
        heap_delete(staging);
        for (Handle *h : fds)
            handle_release(h);
        // Last, because a File handle's ~FileIo closes through the VFS and a
        // redirected one lives in the same block as the pipes.
        io.release();
    }

    u32 pid;
    Stdio io;

    // What keeps the record alive: the stepper's reference, and one more for
    // every syscall server, taken by value so the frame that may still be
    // parked on p.io outlives everything it points at.
    u32 refs = 1;

    // The terminal, while this process has it. Both are the kernel's rather
    // than the program's: a killed process runs no destructor, and ~Proc is
    // reached from exec_process's End on ^C, kill and a destroyed frame alike.
    KeyInput *keys  = nullptr;
    FullScreen *alt = nullptr;

    // The call the host is staging bytes for, not yet issued, and the ones
    // that have been. A process owns them both, so a server task cancelled
    // mid-await leaks nothing.
    Call *staging = nullptr;
    Vec<Call *> calls;
    Channel<Reply, PROC_REPLIES> done;

    // The namespace this process names things in (Concept.md §5.1). Its own,
    // inherited from whoever spawned it: the shell's `cd` moves the shell's,
    // and a child that moves this one moves nobody else's feet.
    String cwd;

    // The environment blob this process was entered with, inherited by a child
    // whose Sys::Spawn names none. Fixed at spawn: there is no setenv.
    String env;

    // What this process started, and who is waiting on it. `dead` is the
    // stepper's End having run: a child that finishes afterwards has nobody
    // left to report to, and the record is only still here because a server has
    // not unwound yet.
    Vec<Child> children;
    u32 wait_any  = 0; // the token a Wait(SYS_WAIT_ANY) is parked on
    u32 depth     = 0; // how many spawns deep this one is
    u32 max_pages = 0; // the memory cap this instance was given, for /proc
    u32 pages     = 0; // and what it has committed, as of its last step
    bool dead     = false;

    i32 exit = 1;
    Vec<Handle *> fds;
};

Proc *proc_find(u32 pid);
bool proc_add(Proc *p);
void proc_remove(Proc *p);
void proc_release(Proc *p);

// A counted reference on a process record, taken by a syscall server as a
// coroutine parameter *by value*. A coroutine's parameter copies are destroyed
// when the frame is, which is after the body's locals — and one of those locals
// may be an awaitable parked on the process's stdio, deregistering from a pipe
// in its destructor (prog.h). So the record, and the block its streams point
// at, is the last thing to go. Ordering, not politeness: the stepper's End runs
// while a cancelled server is still on the ready queue.
struct ProcRef {
    explicit ProcRef(Proc *q) : p(q) { p->refs++; }

    ProcRef(const ProcRef &o) : p(o.p) { p->refs++; }

    ProcRef &operator=(const ProcRef &) = delete;

    ~ProcRef() { proc_release(p); }

    Proc *operator->() const { return p; }

    Proc &operator*() const { return *p; }

    Proc *p;
};

// The call the host is about to make, made on demand: Sys::Stage comes first
// when there is a payload, and sys_async alone when there is not.
Call *proc_staging(Proc &p);

// Room for the bytes the host is about to copy in. Per call rather than one
// buffer per process: with two calls in flight the second would otherwise
// overwrite the first before its server had read it. It never shrinks, because
// a process that wrote 512 bytes once will do it again.
u32 proc_stage(Proc &p, u32 n);

Handle *proc_handle(Proc &p, u32 fd);

// Files `h` in the process's own table and reports the descriptor, or -1 when
// there is no room. A slot that was closed is reused before the table grows.
i32 proc_bind(Proc &p, Handle *h);

// The descriptor a newly opened thing gets, or a negated Error with the handle
// closed: a table with no room must not leave the host object behind.
i32 handle_bind(Proc &p, Handle *h);

// Performs the request the process is parked on, and builds the payload
// _resume will hand back: an i32 status, then any data. Every wait in it is the
// server task's own, so ^C reaches a process through exactly the awaitables it
// reaches a shell builtin through. Err(Cancelled) is a reply nobody builds.
Task<Result<String>> proc_syscall(Proc &p, Call &c);

// Sys::Spawn. Reports the child's pid, or a negated Error — including
// -Cancelled, which the caller turns back into a reply nobody builds.
Task<i32> proc_spawn_child(Proc &p, u32 arg, Str payload);

// u32s appended to a reply in the order the ABI reads them (sysabi.h). False is
// out of memory. Variadic rather than an initializer list: -nostdinc++.
template <class... U>
bool reply_u32(String &reply, U... v)
{
    u8 head[sizeof...(U) * 4];
    usize at = 0;
    ((sys_put_u32(head + at, u32(v)), at += 4), ...);
    return reply.append(Str(reinterpret_cast<const char *>(head), sizeof(head)));
}

// A Task that would not allocate is Err(NoMemory) rather than a crash.
#define CO_CALL(dst, expr)              \
    do {                                \
        auto _co_t = (expr);            \
        if (!_co_t)                     \
            dst = Err(Error::NoMemory); \
        else                            \
            dst = co_await _co_t;       \
    } while (0)

// Again is a stray wake: the awaitable is rebuilt and awaited again.
#define CO_RETRY(dst, expr)                                 \
    do {                                                    \
        dst = Err(Error::Again);                            \
        while (dst.is_err() && dst.error() == Error::Again) \
            dst = co_await (expr);                          \
    } while (0)

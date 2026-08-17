#include "exec.h"

#include "console.h"
#include "fs/hostfs.h"
#include "fs/path.h"
#include "fs/vfs.h"
#include "io.h"
#include "kernel/alloc.h"
#include "kernel/key.h"
#include "kernel/sched.h"
#include "svc/net.h"
#include "svc/proc.h"
#include "svc/svc.h"
#include "svc/xfer.h"
#include "tty.h"

namespace {

// A pipe between two processes: the shell's Pipe on the heap and refcounted,
// because its two ends are two descriptors that close in either order and may
// by then be held by two different processes.
struct ProcPipe {
    u32 refs = 1;
    Pipe ch;
};

void pipe_release(ProcPipe *q)
{
    if (--q->refs == 0)
        heap_delete(q);
}

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

    FileIo file;      // File
    HttpResponse res; // Body
    WebSocket sock;   // Socket
    Picked pick;      // PickSet
    PipeEnd pipe;     // PipeRead, PipeWrite
    u32 set  = 0;     // PickFile: the descriptor of the set it came from
    usize ix = 0;     // PickFile: which of that set's files
    u64 off  = 0;     // Body and PickFile: how far it has been read
};

void handle_release(Handle *h)
{
    if (h && --h->refs == 0)
        heap_delete(h);
}

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

// Lazily built, like the orphan list: a namespace-scope Vec has a destructor,
// and nothing provides __cxa_atexit.
Vec<Proc *> *g_procs;

// System-wide, outliving every process record, so not a member of one.
ExecStats g_stats{};

Proc *proc_find(u32 pid)
{
    if (!g_procs)
        return nullptr;
    for (Proc *p : *g_procs)
        if (p->pid == pid)
            return p;
    return nullptr;
}

bool proc_add(Proc *p)
{
    if (!g_procs)
        g_procs = heap_new<Vec<Proc *>>();
    return g_procs && g_procs->push(p);
}

void proc_remove(Proc *p)
{
    if (!g_procs)
        return;
    for (usize i = 0; i < g_procs->size(); i++)
        if ((*g_procs)[i] == p) {
            g_procs->erase(i);
            return;
        }
}

void proc_release(Proc *p)
{
    if (--p->refs == 0)
        heap_delete(p);
}

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
Call *proc_staging(Proc &p)
{
    if (!p.staging)
        p.staging = heap_new<Call>();
    return p.staging;
}

// Room for the bytes the host is about to copy in. Per call rather than one
// buffer per process: with two calls in flight the second would otherwise
// overwrite the first before its server had read it. It never shrinks, because
// a process that wrote 512 bytes once will do it again.
u32 proc_stage(Proc &p, u32 n)
{
    // A hostile binary can call Sys::Stage directly, so the size it asks for
    // is bounded by the largest payload the ABI has: a blit of the whole grid.
    if (n > SYS_STAGE_MAX)
        return 0;
    Call *c = proc_staging(p);
    if (!c)
        return 0;
    if (n > c->cap) {
        u8 *q = static_cast<u8 *>(heap_alloc(n));
        if (!q)
            return 0;
        heap_free(c->stage);
        c->stage = q;
        c->cap   = heap_block_size(n);
    }
    return host_addr(c->stage);
}

Handle *proc_handle(Proc &p, u32 fd)
{
    if (fd < SYS_FD_MIN || fd - SYS_FD_MIN >= p.fds.size())
        return nullptr;
    return p.fds[fd - SYS_FD_MIN];
}

// Files `h` in the process's own table and reports the descriptor, or -1 when
// there is no room. A slot that was closed is reused before the table grows.
i32 proc_bind(Proc &p, Handle *h)
{
    usize slot = p.fds.size();
    for (usize i = 0; i < p.fds.size(); i++)
        if (!p.fds[i]) {
            slot = i;
            break;
        }
    if (slot == p.fds.size() && !p.fds.push(h))
        return -1;
    p.fds[slot] = h;
    return i32(slot + SYS_FD_MIN);
}

// A path as this process names it, made absolute. Resolution happens here
// rather than in the VFS because the cwd being resolved against is the
// process's and not the shell's; what reaches vfs_* is already absolute, and
// path_resolve ignores its cwd for such a path, so the VFS's own second pass
// costs a copy and changes nothing (fs/path.cpp).
//
// Synchronous, and complete before the caller's first await: p.cwd may move
// under another task of the same process, and a Str viewing it would not.
Result<void> proc_path(Proc &p, Str in, String &out)
{
    return path_resolve(p.cwd.str(), in, out);
}

// A process's flags are its own numbers (sysabi.h), mapped rather than shared:
// the filesystem's are free to move without breaking a compiled binary.
u32 vfs_flags(u32 f)
{
    u32 out = 0;
    if (f & SYS_O_READ)
        out |= O_READ;
    if (f & SYS_O_WRITE)
        out |= O_WRITE;
    if (f & SYS_O_CREATE)
        out |= O_CREATE;
    if (f & SYS_O_TRUNC)
        out |= O_TRUNC;
    if (f & SYS_O_APPEND)
        out |= O_APPEND;
    return out;
}

// ------------------------------------------------------- a spawned child
//
// What a child owns and the syscall that made it does not: the binary, the
// argv it views, the directory it starts in, and any descriptor moved into its
// stdio. Refcounted for the reason the shell's Job is — a syscall server of the
// child's may still be parked on one of these streams a tick after the child
// itself is gone — and it holds a reference to *its* parent's stdio owner, so a
// chain of spawns keeps the whole chain's pipes standing.
struct Spawned {
    ~Spawned()
    {
        for (Handle *h : moved)
            handle_release(h);
        parent_io.release();
    }

    u32 refs   = 1;
    u32 parent = 0;
    u32 pid    = 0;
    Executable exe;
    String blob; // the argv bytes, copied out of the staging block
    Vec<Str> words;
    String cwd;
    Handle *moved[3] = {};
    Stdio parent_io; // retained, for the slots that share rather than move
    Stdio io;        // what the child is entered with; io.owner is this
};

void spawn_release(Spawned *s)
{
    if (--s->refs == 0)
        heap_delete(s);
}

// Stdio::hold for a child's streams. They may come from three different places
// at once — a pipe of the parent's, a file it opened, the console — so the one
// owner they all name is this record, which holds each of those up in turn.
void spawn_hold(void *ctx, bool on)
{
    Spawned *s = static_cast<Spawned *>(ctx);
    if (on)
        s->refs++;
    else
        spawn_release(s);
}

struct SpawnRef {
    explicit SpawnRef(Spawned *q) : s(q) { s->refs++; }

    SpawnRef(const SpawnRef &o) : s(o.s) { s->refs++; }

    SpawnRef &operator=(const SpawnRef &) = delete;

    ~SpawnRef() { spawn_release(s); }

    Spawned *operator->() const { return s; }

    Spawned *s;
};

Task<i32> spawn_run(SpawnRef s);

// A moved descriptor as the stream it becomes for the child. The pipe ends need
// no guard here: a child's stdio is the only user of it, which is what moving
// rather than sharing the descriptor buys.
Source handle_source(Handle &h)
{
    return h.kind == Handle::Kind::File ? file_source(h.file) : pipe_source(h.pipe.q->ch);
}

Stream handle_sink(Handle &h)
{
    return h.kind == Handle::Kind::File ? file_sink(h.file) : pipe_sink(h.pipe.q->ch);
}

// Which entry Sys::Wait and Sys::Kill mean. SYS_WAIT_ANY prefers a child that
// has already finished, so a status the kernel is holding is reported rather
// than parked past.
bool find_child(Proc &p, u32 want, usize &at)
{
    if (want) {
        for (usize i = 0; i < p.children.size(); i++)
            if (p.children[i].pid == want) {
                at = i;
                return true;
            }
        return false;
    }
    bool found = false;
    for (usize i = 0; i < p.children.size(); i++) {
        if (!p.children[i].running) {
            at = i;
            return true;
        }
        if (!found) {
            at    = i;
            found = true;
        }
    }
    return found;
}

// Sys::Spawn. Reports the child's pid, or a negated Error — including
// -Cancelled, which the caller turns back into a reply nobody builds.
Task<i32> proc_spawn_child(Proc &p, Str payload)
{
    if (payload.size() < SYS_SPAWN_HEAD * 4)
        co_return -i32(Error::Invalid);

    const u8 *q = reinterpret_cast<const u8 *>(payload.data());
    u32 want[3] = { sys_get_u32(q), sys_get_u32(q + 4), sys_get_u32(q + 8) };

    // A slot names one of the streams this process was given, or a descriptor
    // of its own to hand over — and never the same descriptor twice, which
    // would be one handle owned in two places.
    if (want[0] != SYS_STDIN && want[0] < SYS_FD_MIN)
        co_return -i32(Error::Invalid);
    for (usize i = 1; i < 3; i++)
        if (want[i] != SYS_STDOUT && want[i] != SYS_STDERR && want[i] < SYS_FD_MIN)
            co_return -i32(Error::Invalid);
    for (usize i = 0; i < 3; i++)
        for (usize k = i + 1; k < 3; k++)
            if (want[i] >= SYS_FD_MIN && want[i] == want[k])
                co_return -i32(Error::Invalid);

    // Every child is an instance with a memory cap of its own, so the bound is
    // on the count and on the depth. NoMemory rather than Again: proc_syscall
    // retries an Again for ever.
    if (p.children.size() >= SYS_CHILD_MAX || p.depth + 1 >= SYS_PROC_DEPTH)
        co_return -i32(Error::NoMemory);

    Str blob   = payload.substr(SYS_SPAWN_HEAD * 4);
    usize argc = argv_count(reinterpret_cast<const u8 *>(blob.data()), blob.size());
    if (argc == 0)
        co_return -i32(Error::Invalid);

    Spawned *s = heap_new<Spawned>();
    if (!s)
        co_return -i32(Error::NoMemory);
    struct Drop {
        ~Drop() { spawn_release(s); }

        Spawned *s;
    } drop{ s };

    // argv is copied rather than viewed: the staging block belongs to the call,
    // and the child holds its words until it exits.
    s->parent    = p.pid;
    s->exe.depth = p.depth + 1;
    if (!s->blob.append(blob) || !s->cwd.assign(p.cwd.str()) || !s->words.reserve(argc))
        co_return -i32(Error::NoMemory);
    const u8 *b = reinterpret_cast<const u8 *>(s->blob.data());
    for (usize i = 0; i < argc; i++)
        if (!s->words.push(argv_at(b, s->blob.size(), i)))
            co_return -i32(Error::NoMemory);

    // Resolved before the descriptors are taken, so a name that turns out not
    // to be a command leaves the parent's table as it found it.
    Task<Result<void>> t = exec_resolve(s->words[0], s->exe, p.cwd.str());
    if (!t)
        co_return -i32(Error::NoMemory);
    if (Result<void> r = co_await t; r.is_err())
        co_return -i32(r.error());

    // From here to the end there is no await, which is what makes the take
    // atomic against another task of this process closing a descriptor.
    //
    // Validated first and taken last, so every refusal below leaves the
    // parent's table as it found it. The duplicate check above is what keeps
    // that safe: two slots naming one descriptor would validate twice and be
    // committed into two moved[] entries, which ~Spawned would release twice.
    Handle *take[3] = {};
    for (usize i = 0; i < 3; i++) {
        if (want[i] < SYS_FD_MIN)
            continue;
        Handle *h = proc_handle(p, want[i]);
        if (!h)
            co_return -i32(Error::Invalid);
        // A stream this end can stand behind: a file, or the right end of a
        // pipe. The host services are read by a round trip rather than through
        // a Source, so they are not stdio whatever they are pointed at.
        Handle::Kind want_kind = i == 0 ? Handle::Kind::PipeRead : Handle::Kind::PipeWrite;
        if (h->kind != Handle::Kind::File && h->kind != want_kind)
            co_return -i32(Error::Perm);
        // Nothing this process is inside a syscall on: moving it would leave a
        // parked reader of the parent's on an end the child now owns, which is
        // the second receiver the move exists to make unrepresentable.
        if (h->refs > 1)
            co_return -i32(Error::Perm);
        take[i] = h;
    }

    // Room for the child's entry before the spawn, so the push below cannot
    // fail once there is a pid to record.
    if (!p.children.reserve(p.children.size() + 1))
        co_return -i32(Error::NoMemory);

    // The scheduler's name is a view into the child's own word store, which
    // outlives the task holding it — the same contract a pipeline stage has.
    // The task is lazy and only queued here, so its stdio is built below,
    // before anything can resume it.
    u32 pid = sched_spawn(spawn_run(SpawnRef(s)), s->words[0]);
    if (!pid)
        co_return -i32(Error::NoMemory);
    if (pid > SYS_PID_MAX) {
        // The op word's argument is 24 bits, so Wait and Kill could not name
        // it. Refusing beats handing back a number that truncates into a pid
        // belonging to somebody else.
        sched_cancel(pid);
        co_return -i32(Error::Unsupported);
    }
    s->pid     = pid;
    s->exe.pid = pid;
    if (!p.children.push(Child{ pid, true, 0, 0 })) {
        sched_cancel(pid);
        co_return -i32(Error::NoMemory);
    }

    for (usize i = 0; i < 3; i++) {
        if (!take[i])
            continue;
        p.fds[want[i] - SYS_FD_MIN] = nullptr;
        s->moved[i]                 = take[i];
    }

    s->parent_io = p.io;
    s->parent_io.retain();
    s->io.hold  = spawn_hold;
    s->io.owner = s;
    s->io.in    = want[0] == SYS_STDIN ? p.io.in : handle_source(*s->moved[0]);
    s->io.out   = want[1] == SYS_STDOUT   ? p.io.out
                  : want[1] == SYS_STDERR ? p.io.err
                                          : handle_sink(*s->moved[1]);
    s->io.err   = want[2] == SYS_STDOUT   ? p.io.out
                  : want[2] == SYS_STDERR ? p.io.err
                                          : handle_sink(*s->moved[2]);
    co_return i32(pid);
}

// The child, as a scheduler job of its own — so ^C, `kill`, `jobs` and /proc
// reach it exactly as they reach a stage of a pipeline, and its pid is the one
// the parent was handed.
Task<i32> spawn_run(SpawnRef s)
{
    i32 status = 130; // cancelled before the instance existed

    // Declared before the body, so the body is destroyed first: the status is
    // reported only once the child's own End has killed the instance and
    // released everything it held.
    struct Report {
        ~Report()
        {
            // A child put in front by Sys::Fg has gone, so the console goes
            // back: ^C must not point at a pid nothing answers to. Only when it
            // is the whole foreground — a pipeline's stages are armed together
            // and the ones still running keep it.
            if (console_fg_count() == 1 && console_fg_has(pid))
                console_fg_clear();

            Proc *par = proc_find(parent);
            if (!par || par->dead)
                return;
            for (Child &ch : par->children) {
                if (ch.pid != pid)
                    continue;
                ch.running = false;
                // A negative status on the wire is an error code, and Sys::Exit
                // takes whatever the program passed it.
                ch.status = *status < 0 || *status > 255 ? 255 : *status;
                u32 token = ch.wait;
                ch.wait   = 0;
                if (!token) {
                    token         = par->wait_any;
                    par->wait_any = 0;
                }
                if (token)
                    sched_wake(token, 0, 0);
                return;
            }
        }

        u32 parent;
        u32 pid;
        i32 *status;
    } rep{ s->parent, s->pid, &status };

    Task<i32> body = exec_process(s->exe, Args{ Span<const Str>(s->words.data(), s->words.size()) },
                                  s->io, s->cwd.str());
    if (!body)
        co_return status;
    status = co_await body;
    co_return status;
}

bool read_leb(const u8 *p, usize n, usize &at, u32 &out)
{
    out       = 0;
    u32 shift = 0;
    for (;;) {
        if (at >= n || shift > 28)
            return false;
        u8 b = p[at++];
        out |= u32(b & 0x7f) << shift;
        if (!(b & 0x80))
            return true;
        shift += 7;
    }
}

Task<void> say(Stream err, Str who, Str what)
{
    co_await err.write("braam: ");
    co_await err.write(who);
    co_await err.write(": ");
    co_await err.write(what);
    co_await err.write("\n");
}

// Every byte Sys::Echo writes goes through the process's own stdout, where
// Sys::Write's go. Again is retried and a short write resumed.
Task<Result<void>> echo_write(Stream out, Str s)
{
    while (!s.empty()) {
        Result<usize> r = Err(Error::Again);
        while (r.is_err() && r.error() == Error::Again)
            r = co_await out.write(s);
        if (r.is_err())
            co_return Err(r.error());
        if (r.value() == 0)
            co_return Err(Error::Io);
        s = s.substr(r.value());
    }
    co_return {};
}

// Performs the request the process is parked on, and builds the payload
// _resume will hand back: an i32 status, then any data. Every wait in here is
// the proxy task's own, so ^C reaches a process through exactly the awaitables
// it reaches a shell builtin through.
Task<Result<String>> proc_syscall(Proc &p, Call &c)
{
    String reply;
    if (!reply.append(Str("\0\0\0\0", 4)))
        co_return Err(Error::NoMemory);

    u32 fd      = sys_op_fd(c.op);
    Str payload = Str(reinterpret_cast<const char *>(c.stage), c.len);
    i32 status  = -i32(Error::Unsupported);

    if (c.len > c.cap) {
        status = -i32(Error::NoMemory); // the staging buffer would not grow
    } else
        switch (sys_op_code(c.op)) {
        case Sys::Write: {
            if (fd == SYS_STDOUT || fd == SYS_STDERR) {
                Stream out      = fd == SYS_STDOUT ? p.io.out : p.io.err;
                Result<usize> r = Err(Error::Again);
                while (r.is_err() && r.error() == Error::Again)
                    r = co_await out.write(payload);
                if (r.is_ok())
                    status = i32(r.value());
                else if (r.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                else
                    status = -i32(r.error());
                break;
            }
            Handle *h = proc_handle(p, fd);
            if (!h) {
                status = -i32(Error::Invalid);
                break;
            }
            if (h->busy_w) {
                status = -i32(Error::Perm);
                break;
            }
            HandleRef hold(h);
            HandleBusy busy(h, true);

            // Writing to a socket sends a message; there is nothing else a
            // descriptor of that kind could mean.
            if (h->kind == Handle::Kind::Socket) {
                Result<void> r = Err(Error::NoMemory);
                if (Task<Result<void>> t = ws_send(h->sock, payload))
                    r = co_await t;
                if (r.is_err() && r.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                status = r.is_ok() ? i32(payload.size()) : -i32(r.error());
                break;
            }
            // The write end of a pipe, which is the same wait a write to
            // stdout is when stdout *is* a pipe — the shell's own stages have
            // been doing it since M4.
            if (h->kind == Handle::Kind::PipeWrite) {
                Stream out      = pipe_sink(h->pipe.q->ch);
                Result<usize> r = Err(Error::Again);
                while (r.is_err() && r.error() == Error::Again)
                    r = co_await out.write(payload);
                if (r.is_ok())
                    status = i32(r.value());
                else if (r.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                else
                    status = -i32(r.error());
                break;
            }
            if (h->kind != Handle::Kind::File) {
                status = -i32(Error::Perm);
                break;
            }
            Result<usize> r =
                vfs_write(h->file.fd, h->file.off, reinterpret_cast<const u8 *>(payload.data()),
                          payload.size());
            if (r.is_ok()) {
                h->file.off += r.value();
                status = i32(r.value());
            } else
                status = -i32(r.error());
            break;
        }

        case Sys::Read: {
            if (fd == SYS_STDIN) {
                Result<String> r = Err(Error::Again);
                while (r.is_err() && r.error() == Error::Again)
                    r = co_await p.io.in.read();
                if (r.is_ok()) {
                    if (!reply.append(r.value().str()))
                        co_return Err(Error::NoMemory);
                    status = i32(r.value().size());
                } else if (r.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                else
                    status = r.error() == Error::Closed ? 0 : -i32(r.error());
                break;
            }
            Handle *h = proc_handle(p, fd);
            if (!h) {
                status = -i32(Error::Invalid);
                break;
            }
            if (h->busy_r) {
                status = -i32(Error::Perm);
                break;
            }
            HandleRef hold(h);
            HandleBusy busy(h, false);

            // The read end of a pipe. Err(Closed) is status 0 here as it is
            // everywhere else: the writer's end went, and that is an end of
            // input rather than a failure.
            if (h->kind == Handle::Kind::PipeRead) {
                Source in        = pipe_source(h->pipe.q->ch);
                Result<String> r = Err(Error::Again);
                while (r.is_err() && r.error() == Error::Again)
                    r = co_await in.read();
                if (r.is_ok()) {
                    if (!reply.append(r.value().str()))
                        co_return Err(Error::NoMemory);
                    status = i32(r.value().size());
                } else if (r.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                else
                    status = r.error() == Error::Closed ? 0 : -i32(r.error());
                break;
            }

            // Everything that is a stream of bytes reads the same way, so a
            // fetched body, a socket and a picked file all arrive through the
            // operation a file already had. An empty chunk is the end of it.
            if (h->kind != Handle::Kind::File) {
                // The set a PickFile reads through is a second descriptor, and
                // another task may close that one instead. Resolved before the
                // await, so a set closed first is still Err(Invalid).
                Handle *set = h->kind == Handle::Kind::PickFile ? proc_handle(p, h->set) : nullptr;
                HandleRef keep(set);

                Result<String> r = Err(Error::Perm);
                if (h->kind == Handle::Kind::Body) {
                    if (Task<Result<String>> t = http_read(h->res))
                        r = co_await t;
                } else if (h->kind == Handle::Kind::Socket) {
                    if (Task<Result<String>> t = ws_recv(h->sock))
                        r = co_await t;
                } else if (h->kind == Handle::Kind::PickFile) {
                    if (!set || set->kind != Handle::Kind::PickSet)
                        r = Err(Error::Invalid);
                    else if (Task<Result<String>> t = pick_read(set->pick, h->ix, h->off))
                        r = co_await t;
                }
                if (r.is_ok()) {
                    h->off += r.value().size();
                    if (!reply.append(r.value().str()))
                        co_return Err(Error::NoMemory);
                    status = i32(r.value().size());
                } else if (r.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                else
                    status = r.error() == Error::Closed ? 0 : -i32(r.error());
                break;
            }

            u8 *block = static_cast<u8 *>(heap_alloc(SYS_CHUNK));
            if (!block) {
                status = -i32(Error::NoMemory);
                break;
            }
            Result<usize> r = vfs_read(h->file.fd, h->file.off, block, SYS_CHUNK);
            if (r.is_ok()) {
                h->file.off += r.value();
                status = i32(r.value());
                if (!reply.append(Str(reinterpret_cast<const char *>(block), r.value())))
                    status = -i32(Error::NoMemory);
            } else
                status = -i32(r.error());
            heap_free(block);
            break;
        }

        case Sys::Open: {
            String abs;
            if (Result<void> a = proc_path(p, payload, abs); a.is_err()) {
                status = -i32(a.error());
                break;
            }
            Task<Result<i32>> t = vfs_open(abs.str(), vfs_flags(sys_op_arg(c.op)));
            if (!t) {
                status = -i32(Error::NoMemory);
                break;
            }
            Result<i32> r = co_await t;
            if (r.is_err()) {
                if (r.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                status = -i32(r.error());
                break;
            }

            // A descriptor is an index into this process's own table, so the
            // number a process holds means nothing in any other one.
            Handle *h = heap_new<Handle>(Handle::Kind::File);
            if (!h) {
                vfs_close(r.value());
                status = -i32(Error::NoMemory);
                break;
            }
            h->file.fd = r.value();

            // Appending is a starting offset, not a mode: nothing below the VFS
            // is seekable, so the position is this side's to keep — and this
            // side is the process's handle, since M8 gave it one.
            if (sys_op_arg(c.op) & SYS_O_APPEND)
                if (Result<u64> at = vfs_size(h->file.fd); at.is_ok())
                    h->file.off = at.value();

            status = proc_bind(p, h);
            if (status < 0) {
                handle_release(h); // closes it
                status = -i32(Error::NoMemory);
            }
            break;
        }

        case Sys::Close: {
            Handle *h = proc_handle(p, fd);
            if (!h) {
                status = -i32(Error::Invalid);
                break;
            }
            // The number is free at once, and what the descriptor is on is shut
            // at once — that is what answers a read parked on it. Only the
            // block, and the externref slot in it, waits for the last call.
            p.fds[fd - SYS_FD_MIN] = nullptr;
            h->shut();
            handle_release(h);
            status = 0;
            break;
        }

        case Sys::Stat: {
            String abs;
            if (Result<void> a = proc_path(p, payload, abs); a.is_err()) {
                status = -i32(a.error());
                break;
            }
            Result<Stat> r = Err(Error::NoMemory);
            if (Task<Result<Stat>> t = vfs_stat(abs.str()))
                r = co_await t;
            if (r.is_err()) {
                if (r.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                status = -i32(r.error());
                break;
            }
            u8 head[12];
            sys_put_u32(head, r.value().kind == NodeKind::Dir ? SYS_KIND_DIR : SYS_KIND_FILE);
            sys_put_u32(head + 4, u32(r.value().size));
            sys_put_u32(head + 8, u32(r.value().size >> 32));
            if (!reply.append(Str(reinterpret_cast<const char *>(head), sizeof(head))))
                co_return Err(Error::NoMemory);
            status = 0;
            break;
        }

        case Sys::List: {
            String abs;
            if (Result<void> a = proc_path(p, payload, abs); a.is_err()) {
                status = -i32(a.error());
                break;
            }
            Result<Vec<Entry>> r = Err(Error::NoMemory);
            if (Task<Result<Vec<Entry>>> t = vfs_list(abs.str()))
                r = co_await t;
            if (r.is_err()) {
                if (r.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                status = -i32(r.error());
                break;
            }

            u8 count[4];
            sys_put_u32(count, u32(r.value().size()));
            if (!reply.append(Str(reinterpret_cast<const char *>(count), sizeof(count))))
                co_return Err(Error::NoMemory);
            for (const Entry &e : r.value()) {
                u8 head[16];
                sys_put_u32(head, e.kind == NodeKind::Dir ? SYS_KIND_DIR : SYS_KIND_FILE);
                sys_put_u32(head + 4, u32(e.size));
                sys_put_u32(head + 8, u32(e.size >> 32));
                sys_put_u32(head + 12, u32(e.name.size()));
                if (!reply.append(Str(reinterpret_cast<const char *>(head), sizeof(head))) ||
                    !reply.append(e.name.str()))
                    co_return Err(Error::NoMemory);
            }
            status = 0;
            break;
        }

        case Sys::MkDir: {
            String abs;
            if (Result<void> a = proc_path(p, payload, abs); a.is_err()) {
                status = -i32(a.error());
                break;
            }
            Result<void> r = Err(Error::NoMemory);
            if (Task<Result<void>> t = vfs_mkdir(abs.str()))
                r = co_await t;
            if (r.is_err() && r.error() == Error::Cancelled)
                co_return Err(Error::Cancelled);
            status = r.is_ok() ? 0 : -i32(r.error());
            break;
        }

        case Sys::Remove: {
            String abs;
            if (Result<void> a = proc_path(p, payload, abs); a.is_err()) {
                status = -i32(a.error());
                break;
            }
            Result<void> r = Err(Error::NoMemory);
            if (Task<Result<void>> t = vfs_remove(abs.str(), sys_op_arg(c.op) & 1))
                r = co_await t;
            if (r.is_err() && r.error() == Error::Cancelled)
                co_return Err(Error::Cancelled);
            status = r.is_ok() ? 0 : -i32(r.error());
            break;
        }

        // Get and set in one operation, as KeyClaim and ScreenEnter are: the
        // answer to both is the resulting cwd, and a program that has just
        // moved wants it as much as one that only asked.
        case Sys::Chdir: {
            if (sys_op_arg(c.op) & 1) {
                String abs;
                if (Result<void> a = proc_path(p, payload, abs); a.is_err()) {
                    status = -i32(a.error());
                    break;
                }
                Result<Stat> r = Err(Error::NoMemory);
                if (Task<Result<Stat>> t = vfs_stat(abs.str()))
                    r = co_await t;
                if (r.is_err()) {
                    if (r.error() == Error::Cancelled)
                        co_return Err(Error::Cancelled);
                    status = -i32(r.error());
                    break;
                }
                if (r.value().kind != NodeKind::Dir) {
                    status = -i32(Error::NotDir);
                    break;
                }
                // Last, so a cwd that could not be stored leaves the old one
                // standing rather than an empty string nothing resolves against.
                if (!p.cwd.assign(abs.str())) {
                    status = -i32(Error::NoMemory);
                    break;
                }
            }
            if (!reply.append(p.cwd.str()))
                co_return Err(Error::NoMemory);
            status = 0;
            break;
        }

        case Sys::Sleep: {
            if (payload.size() < 4) {
                status = -i32(Error::Invalid);
                break;
            }
            Result<void> r = co_await sleep_ms(sys_get_u32(c.stage));
            if (r.is_err())
                co_return Err(Error::Cancelled);
            status = 0;
            break;
        }

        case Sys::Clock: {
            Result<WallClock> r = Err(Error::NoMemory);
            if (Task<Result<WallClock>> t = svc_clock())
                r = co_await t;
            if (r.is_err()) {
                if (r.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                status = -i32(r.error());
                break;
            }
            u8 head[12];
            sys_put_u32(head, u32(r.value().epoch_ms));
            sys_put_u32(head + 4, u32(r.value().epoch_ms >> 32));
            sys_put_u32(head + 8, u32(r.value().tz_min));
            if (!reply.append(Str(reinterpret_cast<const char *>(head), sizeof(head))))
                co_return Err(Error::NoMemory);
            status = 0;
            break;
        }

        case Sys::Storage: {
            StorageBackend b;
            u32 flags = 0;
            if (Task<Result<StorageBackend>> t = storage_info()) {
                Result<StorageBackend> r = co_await t;
                if (r.is_err() && r.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                if (r.is_ok()) {
                    b     = r.value();
                    flags = SYS_STORE_KNOWN;
                }
            }
            if (b.opfs)
                flags |= SYS_STORE_OPFS;
            if (b.sync)
                flags |= SYS_STORE_SYNC;
            if (b.persisted)
                flags |= SYS_STORE_PERSISTED;

            u8 head[20];
            sys_put_u32(head, u32(b.quota));
            sys_put_u32(head + 4, u32(b.quota >> 32));
            sys_put_u32(head + 8, u32(b.usage));
            sys_put_u32(head + 12, u32(b.usage >> 32));
            sys_put_u32(head + 16, flags);
            if (!reply.append(Str(reinterpret_cast<const char *>(head), sizeof(head))))
                co_return Err(Error::NoMemory);
            status = 0;
            break;
        }

        case Sys::Fetch: {
            if (payload.size() < 4) {
                status = -i32(Error::Invalid);
                break;
            }
            u32 url_len = sys_get_u32(c.stage);
            if (usize(url_len) + 4 > payload.size()) {
                status = -i32(Error::Invalid);
                break;
            }
            Str url  = payload.substr(4, url_len);
            Str spec = payload.substr(4 + url_len);

            Result<HttpResponse> r = Err(Error::NoMemory);
            if (Task<Result<HttpResponse>> t = http_fetch(url, spec))
                r = co_await t;
            if (r.is_err()) {
                if (r.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                status = -i32(r.error());
                break;
            }

            Handle *h = heap_new<Handle>(Handle::Kind::Body);
            if (!h) {
                status = -i32(Error::NoMemory);
                break;
            }
            u32 http = r.value().status;
            String headers;
            bool ok = headers.assign(r.value().headers.str());
            h->res  = move(r.value());

            status = proc_bind(p, h);
            if (status < 0 || !ok) {
                handle_release(h);
                status = -i32(Error::NoMemory);
                break;
            }
            u8 head[4];
            sys_put_u32(head, http);
            if (!reply.append(Str(reinterpret_cast<const char *>(head), sizeof(head))) ||
                !reply.append(headers.str()))
                co_return Err(Error::NoMemory);
            break;
        }

        case Sys::WsOpen: {
            Result<WebSocket> r = Err(Error::NoMemory);
            if (Task<Result<WebSocket>> t = ws_open(payload))
                r = co_await t;
            if (r.is_err()) {
                if (r.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                status = -i32(r.error());
                break;
            }

            Handle *h = heap_new<Handle>(Handle::Kind::Socket);
            if (!h) {
                status = -i32(Error::NoMemory);
                break;
            }
            h->sock = move(r.value());
            status  = proc_bind(p, h);
            if (status < 0) {
                handle_release(h);
                status = -i32(Error::NoMemory);
            }
            break;
        }

        case Sys::ClipRead: {
            Result<String> r = Err(Error::NoMemory);
            if (sys_op_arg(c.op) & 1) {
                if (Task<Result<String>> t = clip_wait())
                    r = co_await t;
            } else if (Task<Result<String>> t = clip_read()) {
                r = co_await t;
            }
            if (r.is_err()) {
                if (r.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                status = -i32(r.error());
                break;
            }
            if (!reply.append(r.value().str()))
                co_return Err(Error::NoMemory);
            status = i32(r.value().size());
            break;
        }

        case Sys::ClipWrite: {
            Result<void> r = Err(Error::NoMemory);
            if (Task<Result<void>> t = clip_write(payload))
                r = co_await t;
            if (r.is_err() && r.error() == Error::Cancelled)
                co_return Err(Error::Cancelled);
            status = r.is_ok() ? 0 : -i32(r.error());
            break;
        }

        case Sys::Pick: {
            Result<Picked> r = Err(Error::NoMemory);
            if (Task<Result<Picked>> t = pick_files())
                r = co_await t;
            if (r.is_err()) {
                if (r.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                status = -i32(r.error());
                break;
            }

            Handle *h = heap_new<Handle>(Handle::Kind::PickSet);
            if (!h) {
                status = -i32(Error::NoMemory);
                break;
            }
            usize count = r.value().count;
            h->pick     = move(r.value());
            status      = proc_bind(p, h);
            if (status < 0) {
                handle_release(h);
                status = -i32(Error::NoMemory);
                break;
            }
            HandleRef hold(h); // the loop below awaits, and h is now closeable

            // The names come back with the set, since a program needs every
            // one of them before it opens any: one round trip, not N.
            u8 head[4];
            sys_put_u32(head, u32(count));
            if (!reply.append(Str(reinterpret_cast<const char *>(head), sizeof(head))))
                co_return Err(Error::NoMemory);
            for (usize i = 0; i < count; i++) {
                Result<String> name = Err(Error::NoMemory);
                if (Task<Result<String>> t = pick_name(h->pick, i))
                    name = co_await t;
                if (name.is_err())
                    co_return Err(name.error());
                u8 len[4];
                sys_put_u32(len, u32(name.value().size()));
                if (!reply.append(Str(reinterpret_cast<const char *>(len), sizeof(len))) ||
                    !reply.append(name.value().str()))
                    co_return Err(Error::NoMemory);
            }
            break;
        }

        case Sys::PickOpen: {
            Handle *set = proc_handle(p, sys_op_arg(c.op));
            if (!set || set->kind != Handle::Kind::PickSet || payload.size() < 4) {
                status = -i32(Error::Invalid);
                break;
            }
            usize ix = sys_get_u32(c.stage);
            if (ix >= set->pick.count) {
                status = -i32(Error::NotFound);
                break;
            }

            Handle *h = heap_new<Handle>(Handle::Kind::PickFile);
            if (!h) {
                status = -i32(Error::NoMemory);
                break;
            }

            // By descriptor rather than by pointer: closing the set first is
            // then Err(Invalid) at the next read, not a dangling reference.
            h->set = sys_op_arg(c.op);
            h->ix  = ix;
            status = proc_bind(p, h);
            if (status < 0) {
                handle_release(h);
                status = -i32(Error::NoMemory);
            }
            break;
        }

        case Sys::Save: {
            if (payload.size() < 4) {
                status = -i32(Error::Invalid);
                break;
            }
            u32 name_len = sys_get_u32(c.stage);
            if (usize(name_len) + 4 > payload.size()) {
                status = -i32(Error::Invalid);
                break;
            }
            Result<void> r = Err(Error::NoMemory);
            if (Task<Result<void>> t =
                    save_file(payload.substr(4, name_len), payload.substr(4 + name_len)))
                r = co_await t;
            if (r.is_err() && r.error() == Error::Cancelled)
                co_return Err(Error::Cancelled);
            status = r.is_ok() ? 0 : -i32(r.error());
            break;
        }

        case Sys::KeyClaim:
        case Sys::ScreenEnter: {
            bool take = sys_op_arg(c.op) & 1;
            bool key  = sys_op_code(c.op) == Sys::KeyClaim;

            if (!take) {
                if (key) {
                    heap_delete(p.keys);
                    p.keys = nullptr;
                } else {
                    heap_delete(p.alt);
                    p.alt = nullptr;
                }
            } else if (key ? p.keys != nullptr : p.alt != nullptr) {
                status = -i32(Error::Perm); // this process already holds it
                break;
            } else if (key) {
                // Whether another process holds it is the claim's own answer,
                // so Perm and NoMemory are told apart by the constructor.
                p.keys = heap_new<KeyInput>(p.pid);
                if (!p.keys || !p.keys->ok()) {
                    status = -i32(p.keys ? p.keys->error() : Error::NoMemory);
                    heap_delete(p.keys);
                    p.keys = nullptr;
                    break;
                }
            } else {
                p.alt = heap_new<FullScreen>(p.pid);
                if (!p.alt || !p.alt->ok()) {
                    status = -i32(p.alt ? p.alt->error() : Error::NoMemory);
                    heap_delete(p.alt);
                    p.alt = nullptr;
                    break;
                }
            }

            u8 head[8];
            sys_put_u32(head, screen().cols);
            sys_put_u32(head + 4, screen().rows);
            if (!reply.append(Str(reinterpret_cast<const char *>(head), sizeof(head))))
                co_return Err(Error::NoMemory);
            status = 0;
            break;
        }

        case Sys::KeyRead: {
            if (!p.keys) {
                status = -i32(Error::Perm);
                break;
            }
            Result<Key> r = Err(Error::Again);
            while (r.is_err() && r.error() == Error::Again)
                r = co_await p.keys->next();
            if (r.is_err())
                co_return Err(Error::Cancelled);

            // The geometry rides on every key, so a program that repaints per
            // keystroke handles a resize without an event to subscribe to.
            u8 head[16];
            sys_put_u32(head, r.value().code);
            sys_put_u32(head + 4, r.value().mods);
            sys_put_u32(head + 8, screen().cols);
            sys_put_u32(head + 12, screen().rows);
            if (!reply.append(Str(reinterpret_cast<const char *>(head), sizeof(head))))
                co_return Err(Error::NoMemory);
            status = 0;
            break;
        }

        case Sys::ScreenBlit: {
            // The claim is what a blit is for: a process that does not hold the
            // screen would be painting over whichever one does.
            if (!p.alt) {
                status = -i32(Error::Perm);
                break;
            }
            usize head = SYS_BLIT_HEAD * 4;
            if (payload.size() < head) {
                status = -i32(Error::Invalid);
                break;
            }
            u32 x = sys_get_u32(c.stage), y = sys_get_u32(c.stage + 4);
            u32 w = sys_get_u32(c.stage + 8), h = sys_get_u32(c.stage + 12);
            Cell *cells = screen_cells();
            if (!cells || u64(x) + w > screen().cols || u64(y) + h > screen().rows ||
                payload.size() != head + usize(w) * h * sizeof(Cell)) {
                status = -i32(Error::Invalid);
                break;
            }

            const Cell *from = reinterpret_cast<const Cell *>(c.stage + head);
            for (u32 row = 0; row < h; row++)
                __builtin_memcpy(cells + (y + row) * screen().cols + x, from + row * w,
                                 usize(w) * sizeof(Cell));
            if (w && h)
                screen_touch(x, y, w, h);
            screen_move(sys_get_u32(c.stage + 16), sys_get_u32(c.stage + 20));
            screen_cursor(sys_get_u32(c.stage + 24) != 0);
            status = 0;
            break;
        }

        case Sys::ScreenClear:
            screen_clear();
            status = 0;
            break;

        // The scrolling screen's cursor, which is where a prompt lives. Writing
        // moves it — Sys::Write goes through screen_write, which wraps and
        // scrolls — and nothing counts the scrolls, so a line editor writes and
        // then asks where that landed. A set is refused while somebody else
        // holds the screen, for the reason a blit is.
        case Sys::Cursor: {
            if (sys_op_arg(c.op) & 1) {
                u32 owner = tty_screen_owner();
                if (owner && owner != p.pid) {
                    status = -i32(Error::Perm);
                    break;
                }
                if (payload.size() < 12) {
                    status = -i32(Error::Invalid);
                    break;
                }
                // screen_move clamps, so a column past the edge is the edge
                // rather than a refusal — the deferred wrap column (§3.5) is
                // not one a caller can name.
                screen_move(sys_get_u32(c.stage), sys_get_u32(c.stage + 4));
                screen_cursor(sys_get_u32(c.stage + 8) != 0);
            }

            u8 head[20];
            sys_put_u32(head, screen().cursor_x);
            sys_put_u32(head + 4, screen().cursor_y);
            sys_put_u32(head + 8, screen().cursor_on);
            sys_put_u32(head + 12, screen().cols);
            sys_put_u32(head + 16, screen().rows);
            if (!reply.append(Str(reinterpret_cast<const char *>(head), sizeof(head))))
                co_return Err(Error::NoMemory);
            status = 0;
            break;
        }

        // The colours the next Write paints with. The grid is cells and not a
        // byte stream (§2.3), so a colour cannot ride in the bytes and is a
        // syscall of its own — and it is sticky, so whoever sets one puts the
        // default back. Refused while somebody else holds the screen, as a
        // cursor set is.
        case Sys::Style: {
            u32 owner = tty_screen_owner();
            if (owner && owner != p.pid) {
                status = -i32(Error::Perm);
                break;
            }
            u32 a = sys_op_arg(c.op);
            screen_style(sys_style_fg(a), sys_style_bg(a), sys_style_attrs(a));
            status = 0;
            break;
        }

        // A line editor's whole repaint. The cursor rules are Sys::Cursor's,
        // a run is a Sys::Style and a Sys::Write, every byte goes where
        // Sys::Write's go, and `scrolled` is the answer the caller used to have
        // to go back and ask Cursor for.
        case Sys::Echo: {
            u32 owner = tty_screen_owner();
            if (owner && owner != p.pid) {
                status = -i32(Error::Perm);
                break;
            }
            constexpr usize head = SYS_ECHO_HEAD * 4;
            if (payload.size() < head || !screen().cols) {
                status = -i32(Error::Invalid);
                break;
            }
            u32 x    = sys_get_u32(c.stage);
            u32 y    = sys_get_u32(c.stage + 4);
            u32 cur  = sys_get_u32(c.stage + 8);
            u32 runs = sys_get_u32(c.stage + 12);

            // The whole shape before a cell moves, so a malformed payload
            // paints nothing.
            if (runs > SYS_ECHO_RUNS_MAX) {
                status = -i32(Error::Invalid);
                break;
            }
            usize table = usize(runs) * SYS_ECHO_RUN * 4;
            if (payload.size() < head + table) {
                status = -i32(Error::Invalid);
                break;
            }
            u64 want = u64(head) + table; // a u64: eight u32 lengths overflow one
            for (u32 i = 0; i < runs; i++)
                want += sys_get_u32(c.stage + head + usize(i) * SYS_ECHO_RUN * 4 + 4);
            if (want != payload.size()) {
                status = -i32(Error::Invalid);
                break;
            }

            u32 arg    = sys_op_arg(c.op);
            Stream out = p.io.out;
            Result<void> wrote;

            // Dark for the write, whatever it is left as: one tick, so nothing
            // between here and the placement below is ever presented.
            screen_cursor(false);
            u64 was = screen_scrolled();

            // FRESH: the anchor is wherever the cursor is, on a row of its own.
            // The newline goes through the Stream, so a redirected stdout sees
            // it, and ahead of any run's style, so a scroll blanks the new row
            // in the default colour rather than the prompt's.
            if (arg & SYS_ECHO_FRESH) {
                if (screen().cursor_x != 0) {
                    if (Task<Result<void>> t = echo_write(out, "\n"))
                        wrote = co_await t;
                    else
                        wrote = Err(Error::NoMemory);
                }
            } else
                screen_move(x, y);

            usize at = head + table;
            for (u32 i = 0; wrote.is_ok() && i < runs; i++) {
                const u8 *h = c.stage + head + usize(i) * SYS_ECHO_RUN * 4;
                u32 style   = sys_get_u32(h);
                usize len   = sys_get_u32(h + 4);

                // A run naming no colour paints in the sticky one; one with no
                // bytes only sets the colour.
                if (style != SYS_STYLE_KEEP)
                    screen_style(sys_style_fg(style), sys_style_bg(style), sys_style_attrs(style));
                if (len) {
                    if (Task<Result<void>> t = echo_write(out, payload.substr(at, len)))
                        wrote = co_await t;
                    else
                        wrote = Err(Error::NoMemory);
                }
                at += len;
            }

            u32 scrolled = u32(screen_scrolled() - was);
            if (wrote.is_err()) {
                if (wrote.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                status = -i32(wrote.error());
                break;
            }
            if (arg & SYS_ECHO_END) {
                // The deferred wrap column is not one screen_move can name, so
                // a write that filled a row is carried to the next. That can
                // scroll, so `scrolled` is taken again after it.
                if (screen().cursor_x >= screen().cols) {
                    if (Task<Result<void>> t = echo_write(out, "\n"))
                        wrote = co_await t;
                    else
                        wrote = Err(Error::NoMemory);
                    scrolled = u32(screen_scrolled() - was);
                }
            } else {
                // Where the caller wants the cursor left, measured from the
                // anchor and carried up by whatever the write scrolled under it.
                u32 off = x + cur;
                u32 row = y + off / screen().cols;
                screen_move(off % screen().cols, row >= scrolled ? row - scrolled : 0);
            }
            if (wrote.is_err()) {
                if (wrote.error() == Error::Cancelled)
                    co_return Err(Error::Cancelled);
                status = -i32(wrote.error());
                break;
            }
            screen_cursor((arg & SYS_ECHO_SHOW) != 0);

            u8 at_head[24];
            sys_put_u32(at_head, screen().cursor_x);
            sys_put_u32(at_head + 4, screen().cursor_y);
            sys_put_u32(at_head + 8, screen().cursor_on);
            sys_put_u32(at_head + 12, screen().cols);
            sys_put_u32(at_head + 16, screen().rows);
            sys_put_u32(at_head + 20, scrolled);
            if (!reply.append(Str(reinterpret_cast<const char *>(at_head), sizeof(at_head))))
                co_return Err(Error::NoMemory);
            status = 0;
            break;
        }

        // Both ends in this process's table. Whichever is moved into a child is
        // closed here by the move, and that is what gives the other end an end
        // of input — there is no second copy left open to prevent it.
        case Sys::Pipe: {
            ProcPipe *q = heap_new<ProcPipe>();
            Handle *r   = q ? heap_new<Handle>(Handle::Kind::PipeRead) : nullptr;
            Handle *w   = r ? heap_new<Handle>(Handle::Kind::PipeWrite) : nullptr;
            if (!w) {
                handle_release(r);
                heap_delete(q);
                status = -i32(Error::NoMemory);
                break;
            }
            r->pipe.q      = q;
            w->pipe.q      = q;
            w->pipe.writer = true;
            q->refs        = 2; // one per end, and the ends close in either order

            i32 rfd = proc_bind(p, r);
            i32 wfd = rfd < 0 ? -1 : proc_bind(p, w);
            if (wfd < 0) {
                if (rfd >= 0)
                    p.fds[usize(rfd) - SYS_FD_MIN] = nullptr;
                handle_release(r);
                handle_release(w);
                status = -i32(Error::NoMemory);
                break;
            }
            u8 head[8];
            sys_put_u32(head, u32(rfd));
            sys_put_u32(head + 4, u32(wfd));
            if (!reply.append(Str(reinterpret_cast<const char *>(head), sizeof(head))))
                co_return Err(Error::NoMemory);
            status = 0;
            break;
        }

        case Sys::Spawn: {
            status = -i32(Error::NoMemory);
            if (Task<i32> t = proc_spawn_child(p, payload))
                status = co_await t;
            if (status == -i32(Error::Cancelled))
                co_return Err(Error::Cancelled);
            break;
        }

        // Parks until a child stops, and erases it when it reports. The loop is
        // for the stray wake: being woken is not proof the child this call
        // named is the one that finished.
        case Sys::Wait: {
            u32 want = sys_op_arg(c.op);
            for (;;) {
                usize at = 0;
                if (!find_child(p, want, at)) {
                    status = -i32(Error::NotFound);
                    break;
                }
                if (!p.children[at].running) {
                    u32 pid = p.children[at].pid;
                    i32 st  = p.children[at].status;
                    p.children.erase(at);
                    u8 head[4];
                    sys_put_u32(head, pid);
                    if (!reply.append(Str(reinterpret_cast<const char *>(head), sizeof(head))))
                        co_return Err(Error::NoMemory);
                    status = st;
                    break;
                }

                // One waiter per child, and one for "any": the token is a
                // single slot, and a second would displace the first silently.
                u32 pid = p.children[at].pid;
                if (want ? p.children[at].wait != 0 : p.wait_any != 0) {
                    status = -i32(Error::Perm);
                    break;
                }

                Wake w;
                // By pid rather than by a pointer into the Vec: another task of
                // this process may push a child onto it while this one is
                // parked, and that moves every element.
                struct Clear {
                    ~Clear()
                    {
                        if (want) {
                            usize k = 0;
                            if (find_child(*p, want, k) && p->children[k].wait == token)
                                p->children[k].wait = 0;
                        } else if (p->wait_any == token) {
                            p->wait_any = 0;
                        }
                    }

                    Proc *p;
                    u32 want;
                    u32 token;
                } clear{ &p, want, w.token() };

                if (want)
                    p.children[at].wait = w.token();
                else
                    p.wait_any = w.token();

                if ((co_await w).is_err())
                    co_return Err(Error::Cancelled);
                (void)pid;
            }
            break;
        }

        // Told, not asked, and only about one's own: the child's stepper is a
        // scheduler job, so cancelling it is what `kill %n` and ^C already do.
        case Sys::Kill: {
            usize at = 0;
            if (!find_child(p, sys_op_arg(c.op), at) || sys_op_arg(c.op) == SYS_WAIT_ANY) {
                status = -i32(Error::Perm);
                break;
            }
            if (p.children[at].running)
                sched_cancel(p.children[at].pid);
            status = 0;
            break;
        }

        // Handing the console to a child, which is what a shell does before it
        // waits: ^C then reaches the child rather than the shell that started
        // it. The caller must have the terminal already — it holds the raw keys,
        // or it is itself what is in front — so a background program cannot take
        // ^C away from the prompt.
        case Sys::Fg: {
            // The caller has to have the terminal, or nobody may. A shell lets
            // go of the keyboard before it spawns — the child would otherwise
            // lose the race for it — so "holds the keys" cannot be the whole
            // rule; "and nobody is in front" is what stops a background program
            // taking ^C away from whatever is. Nor is it enough: a pipeline is
            // armed a stage at a time, so what is in front by the second call
            // is what this caller put there, and the foreground belongs to
            // whoever armed it.
            if (tty_keys_owner() != p.pid && !console_fg_has(p.pid) && console_fg_count() &&
                console_fg_owner() != p.pid) {
                status = -i32(Error::Perm);
                break;
            }
            u32 want = sys_op_arg(c.op);
            if (!want) {
                console_fg_clear();
                status = 0;
                break;
            }
            usize at = 0;
            if (!find_child(p, want, at) || !p.children[at].running) {
                status = -i32(Error::Perm);
                break;
            }
            // Added rather than replacing, because the op word carries one pid
            // and a pipeline is up to eight of them: a shell puts its stages in
            // front one call at a time, and ^C reaches all of them.
            status = console_fg_add(want, p.pid) ? 0 : -i32(Error::NoMemory);
            break;
        }

        default:
            break;
        }

    sys_put_u32(reinterpret_cast<u8 *>(reply.data()), u32(status));
    co_return move(reply);
}

// One syscall, performed in a job of its own, reporting back to the stepper.
// The Call stays the process's: this frame may be destroyed while suspended,
// and freeing from here would leave the process holding a dangling pointer.
Task<i32> serve(ProcRef p, Call *c)
{
    // Before the await, not after: on the cancelled path ~Proc has already
    // freed the Call by the time this frame is resumed.
    u32 token = c->token;

    Task<Result<String>> t = proc_syscall(*p, *c);
    Result<String> r       = t ? co_await t : Err(Error::NoMemory);

    Reply rep;
    rep.token = token;
    if (r.is_ok()) {
        rep.payload = move(r.value());
    } else {
        // Cancelled means the process is going anyway, and nobody is left to
        // hear; any other error is the answer.
        if (r.error() == Error::Cancelled)
            co_return 1;
        u8 head[4];
        sys_put_u32(head, u32(-i32(r.error())));
        if (!rep.payload.append(Str(reinterpret_cast<const char *>(head), sizeof(head))))
            co_return 1;
    }

    for (usize i = 0; i < p->calls.size(); i++)
        if (p->calls[i] == c) {
            p->calls.erase(i);
            break;
        }
    heap_delete(c);

    // The box has more slots than a process has tasks, so this never parks.
    p->done.try_send(move(rep));
    co_return 0;
}

// Where a spawn waits. A program *is* a worker now and there is no second place
// to put one, so a host with none to give is waited out rather than failed: 10
// ms, then 20, 50, 100, 200, 500, and a second from there on, with a line each
// time (Concept.md §4). Every other error is the caller's to report.
//
// The image goes with each attempt — the request record owns it past a
// cancelled await — so a retry reads it back rather than keeping a copy the
// first attempt would have paid for.
Task<Result<void>> spawn_process(Executable &exe, u32 pid, Stream err)
{
    constexpr u32 BACKOFF_MS[] = { 10, 20, 50, 100, 200, 500, 1000 };
    constexpr usize LAST       = sizeof(BACKOFF_MS) / sizeof(BACKOFF_MS[0]) - 1;

    for (usize n = 0;; n++) {
        Task<Result<void>> t = proc_spawn(pid, exe.path.str(), move(exe.image), exe.meta);
        if (!t)
            co_return Err(Error::NoMemory);
        Result<void> r = co_await t;
        if (r.is_ok() || r.error() != Error::Again)
            co_return r;

        if (Task<void> s = say(err, exe.path.str(), "no worker, retrying"))
            co_await s;

        Task<Result<void>> w = sleep_ms(BACKOFF_MS[n < LAST ? n : LAST]);
        if (!w)
            co_return Err(Error::NoMemory);
        CO_TRY_VOID(co_await w);

        Task<Result<String>> f = read_file(exe.path.str());
        if (!f)
            co_return Err(Error::NoMemory);
        Result<String> image = co_await f;
        if (image.is_err())
            co_return Err(image.error());
        exe.image = move(image.value());
    }
}

} // namespace

Result<ProcMeta> exec_meta(Str image)
{
    const u8 *p = reinterpret_cast<const u8 *>(image.data());
    usize n     = image.size();
    if (n < 8 || p[0] != 0 || p[1] != 'a' || p[2] != 's' || p[3] != 'm')
        return Err(Error::Invalid);

    for (usize at = 8; at < n;) {
        u32 id = p[at++];
        u32 size;
        if (!read_leb(p, n, at, size) || size > n - at)
            return Err(Error::Invalid);
        usize end = at + size;

        u32 name_len;
        if (id == 0 && read_leb(p, end, at, name_len) && name_len <= end - at) {
            Str name(reinterpret_cast<const char *>(p + at), name_len);
            if (name == PROC_SECTION && end - at - name_len >= sizeof(ProcMeta)) {
                const u8 *q = p + at + name_len;
                ProcMeta m{ sys_get_u32(q), sys_get_u32(q + 4), sys_get_u32(q + 8),
                            sys_get_u32(q + 12), sys_get_u32(q + 16) };
                if (m.magic != PROC_MAGIC)
                    return Err(Error::Invalid);
                // Ours, but built against another kernel. Its own error, not
                // Invalid: §4.3's stale-binary diagnostic is only a diagnostic
                // if it is distinguishable from a file that was never a program.
                if (m.abi != PROC_ABI)
                    return Err(Error::Unsupported);
                return m;
            }
        }
        at = end;
    }
    return Err(Error::Invalid);
}

Task<Result<void>> exec_resolve(Str name, Executable &out, Str cwd)
{
    if (name.empty())
        co_return Err(Error::NotFound);

    // A name with a slash is a path; a bare name is looked for in /bin, which
    // is where the archive puts the binaries. There is no PATH variable yet, and
    // one directory is not a search path.
    String path;
    bool ok = name.contains("/") ? path.assign(name) : path.assign("/bin/") && path.append(name);
    if (!ok)
        co_return Err(Error::NoMemory);

    // `./prog` is relative to whoever asked — the shell for a typed command,
    // the parent for a Sys::Spawn, and those are two different directories now.
    if (!cwd.empty()) {
        String abs;
        CO_TRY_VOID(path_resolve(cwd, path.str(), abs));
        path = move(abs);
    }

    Task<Result<String>> t = read_file(path.str());
    if (!t)
        co_return Err(Error::NoMemory);
    Result<String> image = co_await t;
    if (image.is_err())
        co_return Err(image.error());

    ProcMeta meta = CO_TRY(exec_meta(image.value().str()));

    if (meta.max_pages == 0 || meta.max_pages > PROC_MAX_PAGES)
        meta.max_pages = PROC_MAX_PAGES;

    out.meta  = meta;
    out.path  = move(path);
    out.image = move(image.value());
    co_return {};
}

Task<i32> exec_process(Executable &exe, Args args, Stdio io, Str cwd, bool *died)
{
    // True until the one return that says otherwise, so a path added later
    // reports a death rather than being forgotten.
    if (died)
        *died = true;

    Proc *p = heap_new<Proc>(exe.pid, io);
    if (!p || !proc_add(p)) {
        heap_delete(p);
        co_await io.err.write("braam: out of memory\n");
        co_return 1;
    }
    p->depth     = exe.depth;
    p->max_pages = exe.meta.max_pages;
    p->pages     = exe.meta.initial_pages; // until the first step reports
    if (!p->cwd.assign(cwd.empty() ? vfs_cwd() : cwd)) {
        proc_remove(p);
        proc_release(p);
        co_await io.err.write("braam: out of memory\n");
        co_return 1;
    }

    // A destructor, not straight-line code: the instance has to go when the
    // process is cancelled and when this frame is destroyed while suspended,
    // and those are the two cases a branch would miss.
    struct End {
        ~End()
        {
            // Every server goes with the process. A request nobody will answer
            // would otherwise leak its record for the life of the page, and
            // there is one per parked task rather than one per process now.
            // A process going takes its children with it — §3.6's structured
            // concurrency, put back by hand, exactly as run_line does it for a
            // pipeline. Their own Ends do the same one level down.
            p->dead = true;
            for (Child &ch : p->children)
                if (ch.running)
                    sched_cancel(ch.pid);
            for (Call *c : p->calls)
                if (c->server)
                    sched_cancel(c->server);
            if (spawned)
                proc_kill(p->pid);
            proc_remove(p); // so a syscall arriving after this is NotFound
            // Not a delete: a cancelled server is only queued, not unwound, so
            // the record has to outlive this frame by however long the
            // scheduler takes to resume the last of them.
            proc_release(p);
        }

        Proc *p;
        bool spawned = false;
    } end{ p };

    Task<Result<void>> t = spawn_process(exe, p->pid, io.err);
    if (!t)
        co_return 1;
    if (Result<void> r = co_await t; r.is_err()) {
        if (r.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> s = say(io.err, exe.path.str(), "will not instantiate"))
            co_await s;
        co_return 126;
    }
    end.spawned = true;

    // The first step is _start and carries argv; every one after it is
    // _resume, carrying the answer to a syscall.
    String payload;
    usize n = argv_size(args.v.data(), args.size());
    if (!payload.reserve(n))
        co_return 1;
    for (usize i = 0; i < n; i++)
        payload.push(0);
    argv_encode(args.v.data(), args.size(), reinterpret_cast<u8 *>(payload.data()));

    // The stepper. It never performs a syscall itself: each one gets a
    // scheduler job of its own, because a process with two tasks can be parked
    // on a socket that never answers and on a keystroke at the same time, and
    // serving them in turn would mean the second waited on the first.
    u32 token   = 0; // 0 is _start, which answers nothing
    usize alive = 0; // servers still running
    for (;;) {
        g_stats.steps++; // as issued: a step that fails still cost the hops
        Task<Result<ProcStep>> step = proc_step(p->pid, token, payload.str(), &p->pages);
        if (!step)
            co_return 1;
        Result<ProcStep> s = co_await step;
        if (s.is_err())
            co_return s.error() == Error::Cancelled ? 130 : 1;

        if (s.value() == ProcStep::Exited) {
            if (died)
                *died = false;
            co_return p->exit;
        }
        if (s.value() == ProcStep::Trapped) {
            if (Task<void> t2 = say(io.err, exe.path.str(), "crashed"))
                co_await t2;
            co_return 132;
        }

        // One step can park more than one task: resuming the root may start a
        // second and leave both waiting.
        for (Call *c : p->calls) {
            if (c->server)
                continue;
            c->server = sched_spawn(serve(ProcRef(p), c), exe.path.str());
            if (!c->server)
                co_return 1;
            alive++;
        }

        if (!alive) {
            if (Task<void> t2 = say(io.err, exe.path.str(), "suspended with nothing pending"))
                co_await t2;
            co_return 1;
        }

        Result<Reply> r = Err(Error::Again);
        while (r.is_err() && r.error() == Error::Again)
            r = co_await p->done.recv();
        if (r.is_err())
            co_return r.error() == Error::Cancelled ? 130 : 1;
        alive--;
        token   = r.value().token;
        payload = move(r.value().payload);
    }
}

bool exec_proc_state(u32 pid, ProcState &out)
{
    Proc *p = proc_find(pid);
    if (!p) {
        // Not a process, but perhaps a syscall server of one: those are ordinary
        // jobs, and naming their owner is what keeps /proc legible. Only while
        // the call is outstanding — once it is answered the record is erased and
        // what is left is a coroutine finishing, which is what /proc then says.
        if (g_procs)
            for (Proc *q : *g_procs)
                for (const Call *c : q->calls)
                    if (c->server == pid) {
                        out.ppid = q->pid;
                        return true;
                    }
        return false;
    }

    out.worker    = true;
    out.calls     = u32(p->calls.size());
    out.pages     = p->pages;
    out.max_pages = p->max_pages;
    out.dead      = p->dead;
    out.cwd       = p->cwd.str();

    // A closed descriptor leaves its slot behind as a null, so the slots are
    // not the count.
    out.fds = 0;
    for (Handle *h : p->fds)
        if (h)
            out.fds++;

    // Upwards by search: a record holds its children and not its parent, and
    // this is the same scan the exit status takes to find whom to report to.
    out.ppid = 0;
    if (g_procs)
        for (Proc *q : *g_procs)
            for (const Child &ch : q->children)
                if (ch.pid == pid)
                    out.ppid = q->pid;
    return true;
}

i32 exec_sys(u32 pid, u32 op, u32 a0, u32, u32)
{
    Proc *p = proc_find(pid);
    if (!p)
        return -i32(Error::NotFound);

    g_stats.sysfast++;
    switch (Sys(op)) {
    case Sys::Exit:
        p->exit = i32(a0);
        return 0;
    case Sys::GetPid:
        return i32(pid);
    case Sys::Now:
        return i32(sched_now());
    case Sys::Stage:
        return i32(proc_stage(*p, a0));
    default:
        return -i32(Error::Unsupported);
    }
}

// The token *is* recorded, unlike M8: a process may have several calls parked
// at once, so the kernel names the one it is answering when it steps rather
// than the host remembering the last.
i32 exec_sys_async(u32 pid, u32 op, u32 token, u32 len)
{
    Proc *p = proc_find(pid);
    if (!p)
        return -i32(Error::NotFound);

    Call *c = proc_staging(*p);
    if (!c || !p->calls.push(c)) {
        heap_delete(p->staging);
        p->staging = nullptr;
        return -i32(Error::NoMemory);
    }
    c->op      = op;
    c->len     = len;
    c->token   = token;
    p->staging = nullptr;
    g_stats.syscalls++;
    return 0;
}

void exec_stats(ExecStats &out)
{
    out = g_stats;
}

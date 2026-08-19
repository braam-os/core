#include "console.h"
#include "exec.h"
#include "fs/hostfs.h"
#include "fs/path.h"
#include "fs/vfs.h"
#include "io.h"
#include "kernel/alloc.h"
#include "kernel/key.h"
#include "kernel/sched.h"
#include "proctab.h"
#include "svc/net.h"
#include "svc/svc.h"
#include "svc/xfer.h"
#include "tty.h"

namespace {

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

// The same mapping for node kinds, and for the same reason.
u32 sys_kind_of(NodeKind k)
{
    switch (k) {
    case NodeKind::Dir:
        return SYS_KIND_DIR;
    case NodeKind::Link:
        return SYS_KIND_LINK;
    case NodeKind::File:
        break;
    }
    return SYS_KIND_FILE;
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

// A chunk into the reply: its size is the status, and an end of input is 0.
i32 chunk_reply(const Result<String> &r, String &reply)
{
    if (r.is_err())
        return r.error() == Error::Closed ? 0 : -i32(r.error());
    if (!reply.append(r.value().str()))
        return -i32(Error::NoMemory); // nothing was appended, so the reply stands
    return i32(r.value().size());
}

// The same for a write: what went is the status.
i32 write_status(const Result<usize> &r)
{
    return r.is_ok() ? i32(r.value()) : -i32(r.error());
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
    String env; // the env blob, the caller's own when the spawn named none
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

} // namespace

// The child, as a scheduler job of its own — so ^C, `kill`, `jobs` and /proc
// reach it exactly as they reach a stage of a pipeline, and its pid is the one
// the parent was handed.
namespace {

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
                                  s->io, s->cwd.str(), s->env.str());
    if (!body)
        co_return status;
    status = co_await body;
    co_return status;
}

} // namespace

Task<i32> proc_spawn_child(Proc &p, u32 arg, Str payload)
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

    // The argv blob, then the environment when arg says one follows and the
    // caller's own when it does not.
    Str rest       = payload.substr(SYS_SPAWN_HEAD * 4);
    const u8 *rp   = reinterpret_cast<const u8 *>(rest.data());
    usize argv_len = argv_bytes(rp, rest.size());
    if (argv_len == 0)
        co_return -i32(Error::Invalid);

    Str blob = rest.substr(0, argv_len);
    Str envb = p.env.str();
    if (arg & SYS_SPAWN_ENV) {
        envb = rest.substr(argv_len);
        if (argv_bytes(rp + argv_len, envb.size()) != envb.size())
            co_return -i32(Error::Invalid);
    }
    if (envb.size() > SYS_ENV_MAX)
        co_return -i32(Error::Invalid);

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
    if (!s->blob.append(blob) || !s->env.append(envb) || !s->cwd.assign(p.cwd.str()) ||
        !s->words.reserve(argc))
        co_return -i32(Error::NoMemory);
    const u8 *b = reinterpret_cast<const u8 *>(s->blob.data());
    for (usize i = 0; i < argc; i++)
        if (!s->words.push(argv_at(b, s->blob.size(), i)))
            co_return -i32(Error::NoMemory);

    // Resolved before the descriptors are taken, so a name that turns out not
    // to be a command leaves the parent's table as it found it.
    Task<Result<void>> t = exec_resolve(s->words[0], s->exe, p.cwd.str(), s->env.str());
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
        // the second receiver the move exists to make unrepresentable. The
        // busy flags say exactly that; refs cannot, since Sys::Dup raises it.
        if (h->busy_r || h->busy_w)
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
        // The boundary between the two id spaces: above it is a job of the
        // kernel's own, which this wire has no way to name.
        sched_cancel(pid);
        co_return -i32(Error::Unsupported);
    }
    s->pid     = pid;
    s->exe.pid = pid;

    // The entry outlives the child, so the pid is reserved for as long as it is
    // here: Sys::Wait drops it, and so does ~Proc for one never collected.
    if (!sched_pid_hold(pid)) {
        sched_cancel(pid);
        co_return -i32(Error::NoMemory);
    }
    if (!p.children.push(Child{ pid, true, 0, 0 })) {
        sched_pid_drop(pid);
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
                CO_RETRY(r, out.write(payload));
                status = write_status(r);
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
                CO_CALL(r, ws_send(h->sock, payload));
                status = i32(payload.size());
                if (r.is_err())
                    status = -i32(r.error());
                break;
            }
            // The write end of a pipe, which is the same wait a write to
            // stdout is when stdout *is* a pipe — the shell's own stages have
            // been doing it since M4.
            if (h->kind == Handle::Kind::PipeWrite) {
                Stream out      = pipe_sink(h->pipe.q->ch);
                Result<usize> r = Err(Error::Again);
                CO_RETRY(r, out.write(payload));
                status = write_status(r);
                break;
            }
            if (h->kind != Handle::Kind::File) {
                status = -i32(Error::Perm);
                break;
            }
            Result<usize> r =
                vfs_write(h->file.fd, h->file.off, reinterpret_cast<const u8 *>(payload.data()),
                          payload.size());
            if (r.is_ok())
                h->file.off += r.value();
            status = write_status(r);
            break;
        }

        case Sys::Read: {
            if (fd == SYS_STDIN) {
                Result<String> r = Err(Error::Again);
                CO_RETRY(r, p.io.in.read());
                status = chunk_reply(r, reply);
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
                CO_RETRY(r, in.read());
                status = chunk_reply(r, reply);
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
                    CO_CALL(r, http_read(h->res));
                } else if (h->kind == Handle::Kind::Socket) {
                    CO_CALL(r, ws_recv(h->sock));
                } else if (h->kind == Handle::Kind::PickFile) {
                    if (!set || set->kind != Handle::Kind::PickSet)
                        r = Err(Error::Invalid);
                    else
                        CO_CALL(r, pick_read(set->pick, h->ix, h->off));
                }
                if (r.is_ok())
                    h->off += r.value().size();
                status = chunk_reply(r, reply);
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
            Result<i32> r = Err(Error::NoMemory);
            CO_CALL(r, vfs_open(abs.str(), vfs_flags(sys_op_arg(c.op))));
            if (r.is_err()) {
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

            status = handle_bind(p, h);
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
            // A Dup'd descriptor shuts nothing until the last of them goes.
            p.fds[fd - SYS_FD_MIN] = nullptr;
            if (--h->fds == 0)
                h->shut();
            handle_release(h);
            status = 0;
            break;
        }

        case Sys::Dup: {
            Handle *h = proc_handle(p, fd);
            if (!h) {
                status = -i32(Error::Invalid);
                break;
            }
            i32 nfd = proc_bind(p, h);
            if (nfd < 0) {
                status = -i32(Error::NoMemory);
                break;
            }
            h->refs++;
            h->fds++;
            status = nfd;
            break;
        }

        case Sys::Stat: {
            String abs;
            if (Result<void> a = proc_path(p, payload, abs); a.is_err()) {
                status = -i32(a.error());
                break;
            }
            Result<Stat> r = Err(Error::NoMemory);
            CO_CALL(r, vfs_stat(abs.str(), !(sys_op_arg(c.op) & SYS_STAT_NOFOLLOW)));
            if (r.is_err()) {
                status = -i32(r.error());
                break;
            }
            if (!reply_u32(reply, sys_kind_of(r.value().kind), u32(r.value().size),
                           u32(r.value().size >> 32), u32(r.value().mtime),
                           u32(r.value().mtime >> 32)))
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
            CO_CALL(r, vfs_list(abs.str()));
            if (r.is_err()) {
                status = -i32(r.error());
                break;
            }

            if (!reply_u32(reply, u32(r.value().size())))
                co_return Err(Error::NoMemory);
            for (const Entry &e : r.value())
                if (!reply_u32(reply, sys_kind_of(e.kind), u32(e.size), u32(e.size >> 32),
                               u32(e.mtime), u32(e.mtime >> 32), u32(e.name.size())) ||
                    !reply.append(e.name.str()))
                    co_return Err(Error::NoMemory);
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
            CO_CALL(r, vfs_mkdir(abs.str()));
            status = 0;
            if (r.is_err())
                status = -i32(r.error());
            break;
        }

        case Sys::Remove: {
            String abs;
            if (Result<void> a = proc_path(p, payload, abs); a.is_err()) {
                status = -i32(a.error());
                break;
            }
            Result<void> r = Err(Error::NoMemory);
            CO_CALL(r, vfs_remove(abs.str(), sys_op_arg(c.op) & 1));
            status = 0;
            if (r.is_err())
                status = -i32(r.error());
            break;
        }

        case Sys::Touch: {
            String abs;
            if (Result<void> a = proc_path(p, payload, abs); a.is_err()) {
                status = -i32(a.error());
                break;
            }
            Result<void> r = Err(Error::NoMemory);
            CO_CALL(r, vfs_touch(abs.str()));
            status = 0;
            if (r.is_err())
                status = -i32(r.error());
            break;
        }

        case Sys::Symlink: {
            if (payload.size() < 4) {
                status = -i32(Error::Invalid);
                break;
            }
            u32 target_len = sys_get_u32(c.stage);
            if (usize(target_len) + 4 > payload.size()) {
                status = -i32(Error::Invalid);
                break;
            }
            Str target = payload.substr(4, target_len);

            // The target is stored as written, so it is not made absolute.
            String abs;
            if (Result<void> a = proc_path(p, payload.substr(4 + target_len), abs); a.is_err()) {
                status = -i32(a.error());
                break;
            }
            Result<void> r = Err(Error::NoMemory);
            CO_CALL(r, vfs_symlink(target, abs.str()));
            status = r.is_err() ? -i32(r.error()) : 0;
            break;
        }

        case Sys::ReadLink: {
            String abs;
            if (Result<void> a = proc_path(p, payload, abs); a.is_err()) {
                status = -i32(a.error());
                break;
            }
            Result<String> r = Err(Error::NoMemory);
            CO_CALL(r, vfs_readlink(abs.str()));
            if (r.is_err()) {
                status = -i32(r.error());
                break;
            }
            if (!reply.append(r.value().str()))
                co_return Err(Error::NoMemory);
            status = 0;
            break;
        }

        // Symlink's two-path payload, but both paths are paths here — a link
        // target is stored as written and a destination is not.
        case Sys::Rename: {
            if (payload.size() < 4) {
                status = -i32(Error::Invalid);
                break;
            }
            u32 from_len = sys_get_u32(c.stage);
            if (usize(from_len) + 4 > payload.size()) {
                status = -i32(Error::Invalid);
                break;
            }

            String from, to;
            if (Result<void> a = proc_path(p, payload.substr(4, from_len), from); a.is_err()) {
                status = -i32(a.error());
                break;
            }
            if (Result<void> a = proc_path(p, payload.substr(4 + from_len), to); a.is_err()) {
                status = -i32(a.error());
                break;
            }
            Result<void> r = Err(Error::NoMemory);
            CO_CALL(r, vfs_rename(from.str(), to.str()));
            status = r.is_err() ? -i32(r.error()) : 0;
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
                CO_CALL(r, vfs_stat(abs.str()));
                if (r.is_err()) {
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
            if (r.is_err()) {
                status = -i32(Error::Cancelled);
                break;
            }
            status = 0;
            break;
        }

        case Sys::Clock: {
            Result<WallClock> r = Err(Error::NoMemory);
            CO_CALL(r, svc_clock());
            if (r.is_err()) {
                status = -i32(r.error());
                break;
            }
            if (!reply_u32(reply, u32(r.value().epoch_ms), u32(r.value().epoch_ms >> 32),
                           u32(r.value().tz_min)))
                co_return Err(Error::NoMemory);
            status = 0;
            break;
        }

        case Sys::Storage: {
            StorageBackend b;
            u32 flags                = 0;
            Result<StorageBackend> r = Err(Error::NoMemory);
            CO_CALL(r, storage_info());
            if (r.is_err() && r.error() == Error::Cancelled) {
                status = -i32(Error::Cancelled);
                break;
            }
            if (r.is_ok()) {
                b     = r.value();
                flags = SYS_STORE_KNOWN;
            }
            if (b.opfs)
                flags |= SYS_STORE_OPFS;
            if (b.sync)
                flags |= SYS_STORE_SYNC;
            if (b.persisted)
                flags |= SYS_STORE_PERSISTED;

            if (!reply_u32(reply, u32(b.quota), u32(b.quota >> 32), u32(b.usage),
                           u32(b.usage >> 32), flags))
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
            CO_CALL(r, http_fetch(url, spec));
            if (r.is_err()) {
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

            // Before the bind, not after: releasing a handle the table already
            // points at would leave the descriptor on a freed block.
            if (!ok) {
                handle_release(h);
                status = -i32(Error::NoMemory);
                break;
            }
            status = handle_bind(p, h);
            if (status < 0)
                break;
            if (!reply_u32(reply, http) || !reply.append(headers.str()))
                co_return Err(Error::NoMemory);
            break;
        }

        case Sys::WsOpen: {
            Result<WebSocket> r = Err(Error::NoMemory);
            CO_CALL(r, ws_open(payload));
            if (r.is_err()) {
                status = -i32(r.error());
                break;
            }

            Handle *h = heap_new<Handle>(Handle::Kind::Socket);
            if (!h) {
                status = -i32(Error::NoMemory);
                break;
            }
            h->sock = move(r.value());
            status  = handle_bind(p, h);
            break;
        }

        case Sys::ClipRead: {
            Result<String> r = Err(Error::NoMemory);
            if (sys_op_arg(c.op) & 1) {
                CO_CALL(r, clip_wait());
            } else {
                CO_CALL(r, clip_read());
            }
            if (r.is_err()) {
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
            CO_CALL(r, clip_write(payload));
            status = 0;
            if (r.is_err())
                status = -i32(r.error());
            break;
        }

        case Sys::Pick: {
            Result<Picked> r = Err(Error::NoMemory);
            CO_CALL(r, pick_files());
            if (r.is_err()) {
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
            status      = handle_bind(p, h);
            if (status < 0)
                break;
            HandleRef hold(h); // the loop below awaits, and h is now closeable

            // The names come back with the set, since a program needs every
            // one of them before it opens any: one round trip, not N.
            if (!reply_u32(reply, u32(count)))
                co_return Err(Error::NoMemory);
            for (usize i = 0; i < count; i++) {
                Result<String> name = Err(Error::NoMemory);
                CO_CALL(name, pick_name(h->pick, i));
                if (name.is_err())
                    co_return Err(name.error());
                if (!reply_u32(reply, u32(name.value().size())) ||
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
            status = handle_bind(p, h);
            break;
        }

        case Sys::Fexport: {
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
            CO_CALL(r, fexport_file(payload.substr(4, name_len), payload.substr(4 + name_len)));
            status = 0;
            if (r.is_err())
                status = -i32(r.error());
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

            if (!reply_u32(reply, screen().cols, screen().rows))
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
            CO_RETRY(r, p.keys->next());
            if (r.is_err()) {
                status = -i32(Error::Cancelled);
                break;
            }

            // The geometry rides on every key, so a program that repaints per
            // keystroke handles a resize without an event to subscribe to.
            if (!reply_u32(reply, r.value().code, r.value().mods, screen().cols, screen().rows))
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

            if (!reply_u32(reply, screen().cursor_x, screen().cursor_y, screen_cursor_on(),
                           screen().cols, screen().rows))
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
                    CO_CALL(wrote, echo_write(out, "\n"));
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
                if (len)
                    CO_CALL(wrote, echo_write(out, payload.substr(at, len)));
                at += len;
            }

            u32 scrolled = u32(screen_scrolled() - was);
            if (wrote.is_err()) {
                status = -i32(wrote.error());
                break;
            }
            if (arg & SYS_ECHO_END) {
                // The deferred wrap column is not one screen_move can name, so
                // a write that filled a row is carried to the next. That can
                // scroll, so `scrolled` is taken again after it.
                if (screen().cursor_x >= screen().cols) {
                    CO_CALL(wrote, echo_write(out, "\n"));
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
                status = -i32(wrote.error());
                break;
            }
            screen_cursor((arg & SYS_ECHO_SHOW) != 0);

            if (!reply_u32(reply, screen().cursor_x, screen().cursor_y, screen_cursor_on(),
                           screen().cols, screen().rows, scrolled))
                co_return Err(Error::NoMemory);
            status = 0;
            break;
        }

        // Whether a descriptor is the terminal, and its geometry. A get with no
        // state, so none of Cursor's refusals apply. Zero geometry when the
        // answer is no: a pipe has no width.
        case Sys::Tty: {
            bool console = false;
            if (fd == SYS_STDIN) {
                console = console_is_input(p.io.in);
            } else if (fd == SYS_STDOUT || fd == SYS_STDERR) {
                console = tty_is_console(fd == SYS_STDOUT ? p.io.out : p.io.err);
            } else if (!proc_handle(p, fd)) {
                status = -i32(Error::Invalid);
                break;
            }
            // Anything in the process's own table is a file, a pipe, a socket
            // or a pick set; none is the grid.
            if (!reply_u32(reply, console ? SYS_TTY_CONSOLE : 0u, console ? screen().cols : 0u,
                           console ? screen().rows : 0u))
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
            if (!reply_u32(reply, u32(rfd), u32(wfd)))
                co_return Err(Error::NoMemory);
            status = 0;
            break;
        }

        case Sys::Spawn: {
            status = -i32(Error::NoMemory);
            if (Task<i32> t = proc_spawn_child(p, sys_op_arg(c.op), payload))
                status = co_await t;
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
                    sched_pid_drop(pid); // nothing names it now
                    if (!reply_u32(reply, pid))
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

                if ((co_await w).is_err()) {
                    status = -i32(Error::Cancelled);
                    break;
                }
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

    // Cancelled is not a status, wherever in the switch it came from: the
    // process is going anyway and there is nobody left to hand a reply to.
    if (status == -i32(Error::Cancelled))
        co_return Err(Error::Cancelled);

    sys_put_u32(reinterpret_cast<u8 *>(reply.data()), u32(status));
    co_return move(reply);
}

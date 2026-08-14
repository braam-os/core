#include "exec.h"

#include "fs/hostfs.h"
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

// A descriptor, whatever is behind it. The kinds beyond File are the host
// services that hand back a JS object: a fetch body, a socket, a set of picked
// files. Making them descriptors is what lets `read`, `write` and `close`
// serve all of them — six operations the ABI does not need — and what makes a
// killed process drop them, since ~Handle releases the externref slot and the
// host object with it, with no code of its own to reach.
struct Handle {
    enum class Kind : u8 { File, Body, Socket, PickSet, PickFile };

    explicit Handle(Kind k) : kind(k) {}

    Handle(const Handle &)            = delete;
    Handle &operator=(const Handle &) = delete;

    Kind kind;
    FileIo file;      // File
    HttpResponse res; // Body
    WebSocket sock;   // Socket
    Picked pick;      // PickSet
    u32 set  = 0;     // PickFile: the descriptor of the set it came from
    usize ix = 0;     // PickFile: which of that set's files
    u64 off  = 0;     // Body and PickFile: how far it has been read
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

constexpr usize PROC_REPLIES = 8; // more than PROC_TASKS, so a send never parks

// The kernel's side of one running process. The instance itself is the host's
// — the kernel holds what only the kernel can hold: the stdio the stage was
// given, the descriptors the process opened, and the calls it is waiting on.
struct Proc {
    Proc(u32 p, Stdio s) : pid(p), io(s) {}

    ~Proc()
    {
        heap_delete(alt); // restores the screen
        heap_delete(keys);
        for (Call *c : calls)
            heap_delete(c);
        heap_delete(staging);
        for (Handle *h : fds)
            heap_delete(h);
    }

    u32 pid;
    Stdio io;

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

    i32 exit = 1;
    Vec<Handle *> fds;
};

// Lazily built, like the orphan list: a namespace-scope Vec has a destructor,
// and nothing provides __cxa_atexit.
Vec<Proc *> *g_procs;

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

Task<Result<String>> read_file(Str path)
{
    Task<Result<i32>> t = vfs_open(path, O_READ);
    if (!t)
        co_return Err(Error::NoMemory);
    i32 fd = CO_TRY(co_await t);

    // The staging block is on the heap rather than in this frame: FS_BLOCK is
    // the allocator's top size class, and a frame that big costs a whole span
    // (Concept.md §8.2).
    u8 *block = static_cast<u8 *>(heap_alloc(FS_BLOCK));
    String out;
    Result<void> bad = {};
    if (!block)
        bad = Err(Error::NoMemory);

    for (u64 off = 0; block;) {
        Result<usize> r = vfs_read(fd, off, block, FS_BLOCK);
        if (r.is_err()) {
            bad = Err(r.error());
            break;
        }
        if (r.value() == 0)
            break;
        if (!out.append(Str(reinterpret_cast<const char *>(block), r.value()))) {
            bad = Err(Error::NoMemory);
            break;
        }
        off += r.value();
    }

    heap_free(block);
    vfs_close(fd);
    if (bad.is_err())
        co_return Err(bad.error());
    co_return move(out);
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

// Performs the request the process is parked on, and builds the payload
// _resume will hand back: an i32 status, then any data. Every wait in here is
// the proxy task's own, so ^C reaches a process through exactly the awaitables
// it reaches an applet through.
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

            // Everything that is a stream of bytes reads the same way, so a
            // fetched body, a socket and a picked file all arrive through the
            // operation a file already had. An empty chunk is the end of it.
            if (h->kind != Handle::Kind::File) {
                Result<String> r = Err(Error::Perm);
                if (h->kind == Handle::Kind::Body) {
                    if (Task<Result<String>> t = http_read(h->res))
                        r = co_await t;
                } else if (h->kind == Handle::Kind::Socket) {
                    if (Task<Result<String>> t = ws_recv(h->sock))
                        r = co_await t;
                } else if (h->kind == Handle::Kind::PickFile) {
                    Handle *set = proc_handle(p, h->set);
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
            Task<Result<i32>> t = vfs_open(payload, vfs_flags(sys_op_arg(c.op)));
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
            status     = proc_bind(p, h);
            if (status < 0) {
                heap_delete(h); // closes it
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
            heap_delete(h);
            p.fds[fd - SYS_FD_MIN] = nullptr;
            status                 = 0;
            break;
        }

        case Sys::Stat: {
            Result<Stat> r = Err(Error::NoMemory);
            if (Task<Result<Stat>> t = vfs_stat(payload))
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
            Result<Vec<Entry>> r = Err(Error::NoMemory);
            if (Task<Result<Vec<Entry>>> t = vfs_list(payload))
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
            Result<void> r = Err(Error::NoMemory);
            if (Task<Result<void>> t = vfs_mkdir(payload))
                r = co_await t;
            if (r.is_err() && r.error() == Error::Cancelled)
                co_return Err(Error::Cancelled);
            status = r.is_ok() ? 0 : -i32(r.error());
            break;
        }

        case Sys::Remove: {
            Result<void> r = Err(Error::NoMemory);
            if (Task<Result<void>> t = vfs_remove(payload, sys_op_arg(c.op) & 1))
                r = co_await t;
            if (r.is_err() && r.error() == Error::Cancelled)
                co_return Err(Error::Cancelled);
            status = r.is_ok() ? 0 : -i32(r.error());
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
                heap_delete(h);
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
                heap_delete(h);
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
                heap_delete(h);
                status = -i32(Error::NoMemory);
                break;
            }

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
                heap_delete(h);
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
                status = -i32(Error::Perm); // one claim of each, per process
                break;
            } else if (key) {
                p.keys = heap_new<KeyInput>();
                if (!p.keys || !p.keys->ok()) {
                    heap_delete(p.keys);
                    p.keys = nullptr;
                    status = -i32(Error::NoMemory);
                    break;
                }
            } else {
                p.alt = heap_new<FullScreen>();
                if (!p.alt || !p.alt->ok()) {
                    heap_delete(p.alt);
                    p.alt  = nullptr;
                    status = -i32(Error::NoMemory);
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

        default:
            break;
        }

    sys_put_u32(reinterpret_cast<u8 *>(reply.data()), u32(status));
    co_return move(reply);
}

// One syscall, performed in a job of its own, reporting back to the stepper.
// The Call stays the process's: this frame may be destroyed while suspended,
// and freeing from here would leave the process holding a dangling pointer.
Task<i32> serve(Proc *p, Call *c)
{
    Task<Result<String>> t = proc_syscall(*p, *c);
    Result<String> r       = t ? co_await t : Err(Error::NoMemory);

    Reply rep;
    rep.token = c->token;
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
                ProcMeta m{ sys_get_u32(q),      sys_get_u32(q + 4),  sys_get_u32(q + 8),
                            sys_get_u32(q + 12), sys_get_u32(q + 16), sys_get_u32(q + 20) };
                if (m.magic != PROC_MAGIC || m.abi != PROC_ABI)
                    return Err(Error::Invalid);
                return m;
            }
        }
        at = end;
    }
    return Err(Error::Invalid);
}

Task<Result<void>> exec_resolve(Str name, Executable &out)
{
    // A builtin shadows everything: `cd` is the shell's, whatever a file of
    // that name in /bin might claim.
    if (const Builtin *b = builtin_find(name)) {
        out.builtin = b;
        co_return {};
    }
    if (name.empty())
        co_return Err(Error::NotFound);

    // A name with a slash is a path; a bare name is looked for in /bin, which
    // is where the bundle puts the binaries. There is no PATH variable yet, and
    // one directory is not a search path.
    String path;
    bool ok = name.contains("/") ? path.assign(name) : path.assign("/bin/") && path.append(name);
    if (!ok)
        co_return Err(Error::NoMemory);

    Task<Result<String>> t = read_file(path.str());
    if (!t)
        co_return Err(Error::NoMemory);
    Result<String> image = co_await t;
    if (image.is_err())
        co_return Err(image.error());

    ProcMeta meta = CO_TRY(exec_meta(image.value().str()));

    // A binary claiming tier 1 is a contradiction — an applet has no binary.
    // Tier 3 is asked for here and granted by the host, which falls back to
    // tier 2 where it cannot make a worker (Concept.md §4).
    if (meta.tier != u32(Tier::Instance) && meta.tier != u32(Tier::Worker))
        co_return Err(Error::Invalid);
    if (meta.max_pages == 0 || meta.max_pages > PROC_MAX_PAGES)
        meta.max_pages = PROC_MAX_PAGES;

    out.tier  = Tier(meta.tier);
    out.meta  = meta;
    out.path  = move(path);
    out.image = move(image.value());
    co_return {};
}

Task<i32> exec_process(Executable &exe, Args args, Stdio io)
{
    Proc *p = heap_new<Proc>(exe.pid, io);
    if (!p || !proc_add(p)) {
        heap_delete(p);
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
            for (Call *c : p->calls)
                if (c->server)
                    sched_cancel(c->server);
            if (spawned)
                proc_kill(p->pid);
            proc_remove(p);
            heap_delete(p);
        }

        Proc *p;
        bool spawned = false;
    } end{ p };

    Task<Result<void>> t = proc_spawn(p->pid, exe.path.str(), move(exe.image), exe.meta, exe.tier);
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
        Task<Result<ProcStep>> step = proc_step(p->pid, token, payload.str());
        if (!step)
            co_return 1;
        Result<ProcStep> s = co_await step;
        if (s.is_err())
            co_return s.error() == Error::Cancelled ? 130 : 1;

        if (s.value() == ProcStep::Exited)
            co_return p->exit;
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
            c->server = sched_spawn(serve(p, c), exe.path.str());
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

i32 exec_sys(u32 pid, u32 op, u32 a0, u32, u32)
{
    Proc *p = proc_find(pid);
    if (!p)
        return -i32(Error::NotFound);

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
    return 0;
}

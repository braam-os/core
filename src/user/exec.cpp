#include "exec.h"

#include "fs/vfs.h"
#include "io.h"
#include "kernel/alloc.h"
#include "kernel/sched.h"
#include "svc/proc.h"

namespace {

// The kernel's side of one running process. The instance itself is the host's
// — the kernel holds what only the kernel can hold: the stdio the stage was
// given, the descriptors the process opened, and the one request it is waiting
// on. There is at most one, because a process has a single task.
struct Proc {
    Proc(u32 p, Stdio s) : pid(p), io(s) {}

    ~Proc()
    {
        heap_free(stage);
        for (FileIo *f : fds)
            heap_delete(f);
    }

    u32 pid;
    Stdio io;
    u8 *stage       = nullptr; // where the host copies a syscall's payload
    usize stage_cap = 0;
    u32 op          = 0;
    u32 len         = 0;
    bool pending    = false;
    i32 exit        = 1;
    Vec<FileIo *> fds;
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

// Room for the bytes the host is about to copy in. It never shrinks, because a
// process that wrote 512 bytes once will do it again.
u32 proc_stage(Proc &p, u32 n)
{
    if (n > p.stage_cap) {
        u8 *q = static_cast<u8 *>(heap_alloc(n));
        if (!q)
            return 0;
        heap_free(p.stage);
        p.stage     = q;
        p.stage_cap = heap_block_size(n);
    }
    return host_addr(p.stage);
}

FileIo *proc_file(Proc &p, u32 fd)
{
    if (fd < SYS_FD_MIN || fd - SYS_FD_MIN >= p.fds.size())
        return nullptr;
    return p.fds[fd - SYS_FD_MIN];
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
Task<Result<String>> proc_syscall(Proc &p)
{
    String reply;
    if (!reply.append(Str("\0\0\0\0", 4)))
        co_return Err(Error::NoMemory);

    u32 fd      = sys_op_fd(p.op);
    Str payload = Str(reinterpret_cast<const char *>(p.stage), p.len);
    i32 status  = -i32(Error::Unsupported);

    if (p.len > p.stage_cap) {
        status = -i32(Error::NoMemory); // the staging buffer would not grow
    } else
        switch (sys_op_code(p.op)) {
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
            FileIo *f = proc_file(p, fd);
            if (!f) {
                status = -i32(Error::Invalid);
                break;
            }
            Result<usize> r = vfs_write(f->fd, f->off, reinterpret_cast<const u8 *>(payload.data()),
                                        payload.size());
            if (r.is_ok()) {
                f->off += r.value();
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
            FileIo *f = proc_file(p, fd);
            if (!f) {
                status = -i32(Error::Invalid);
                break;
            }
            u8 *block = static_cast<u8 *>(heap_alloc(SYS_CHUNK));
            if (!block) {
                status = -i32(Error::NoMemory);
                break;
            }
            Result<usize> r = vfs_read(f->fd, f->off, block, SYS_CHUNK);
            if (r.is_ok()) {
                f->off += r.value();
                status = i32(r.value());
                if (!reply.append(Str(reinterpret_cast<const char *>(block), r.value())))
                    status = -i32(Error::NoMemory);
            } else
                status = -i32(r.error());
            heap_free(block);
            break;
        }

        case Sys::Open: {
            if (payload.size() < 4) {
                status = -i32(Error::Invalid);
                break;
            }
            Task<Result<i32>> t = vfs_open(payload.substr(4), vfs_flags(sys_get_u32(p.stage)));
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
            FileIo *f  = heap_new<FileIo>(r.value());
            usize slot = p.fds.size();
            for (usize i = 0; i < p.fds.size(); i++)
                if (!p.fds[i]) {
                    slot = i;
                    break;
                }
            if (!f || (slot == p.fds.size() && !p.fds.push(f))) {
                if (f)
                    heap_delete(f); // closes it
                else
                    vfs_close(r.value());
                status = -i32(Error::NoMemory);
                break;
            }
            p.fds[slot] = f;
            status      = i32(slot + SYS_FD_MIN);
            break;
        }

        case Sys::Close: {
            FileIo *f = proc_file(p, fd);
            if (!f) {
                status = -i32(Error::Invalid);
                break;
            }
            heap_delete(f);
            p.fds[fd - SYS_FD_MIN] = nullptr;
            status                 = 0;
            break;
        }

        default:
            break;
        }

    sys_put_u32(reinterpret_cast<u8 *>(reply.data()), u32(status));
    co_return move(reply);
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
    if (const Program *p = program_find(name)) {
        out.tier   = Tier::Applet;
        out.applet = p;
        co_return {};
    }
    if (name.empty())
        co_return Err(Error::NotFound);

    // A name with a slash is a path; a bare name is looked for in /usr/bin,
    // which is where the bundle puts the binaries. There is no PATH variable
    // yet, and one directory is not a search path.
    String path;
    bool ok =
        name.contains("/") ? path.assign(name) : path.assign("/usr/bin/") && path.append(name);
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

    for (;;) {
        Task<Result<ProcStep>> step = proc_step(p->pid, payload.str());
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
        if (!p->pending) {
            if (Task<void> t2 = say(io.err, exe.path.str(), "suspended with nothing pending"))
                co_await t2;
            co_return 1;
        }
        p->pending = false;

        Task<Result<String>> call = proc_syscall(*p);
        if (!call)
            co_return 1;
        Result<String> reply = co_await call;
        if (reply.is_err())
            co_return reply.error() == Error::Cancelled ? 130 : 1;
        payload = move(reply.value());
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

// The token is not recorded: it is the process's, the host keeps it for the
// _resume that answers, and a process has one outstanding call at a time.
i32 exec_sys_async(u32 pid, u32 op, u32, u32 len)
{
    Proc *p = proc_find(pid);
    if (!p)
        return -i32(Error::NotFound);
    p->op      = op;
    p->len     = len;
    p->pending = true;
    return 0;
}

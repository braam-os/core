#include "exec.h"

#include "fs/path.h"
#include "fs/vfs.h"
#include "io.h"
#include "kernel/alloc.h"
#include "kernel/sched.h"
#include "proctab.h"
#include "svc/proc.h"

namespace {

// System-wide, outliving every process record, so not a member of one.
ExecStats g_stats{};

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
        Result<void> r = Err(Error::NoMemory);
        CO_CALL(r, proc_spawn(pid, exe.path.str(), move(exe.image), exe.meta));
        if (r.is_ok() || r.error() != Error::Again)
            co_return r;

        if (Task<void> s = say(err, exe.path.str(), "no worker, retrying"))
            co_await s;

        Task<Result<void>> w = sleep_ms(BACKOFF_MS[n < LAST ? n : LAST]);
        if (!w)
            co_return Err(Error::NoMemory);
        CO_TRY_VOID(co_await w);

        Result<String> image = Err(Error::NoMemory);
        CO_CALL(image, read_file(exe.path.str()));
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

    Result<String> image = Err(Error::NoMemory);
    CO_CALL(image, read_file(path.str()));
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

Task<i32> exec_process(Executable &exe, Args args, Stdio io, Str cwd, Str env, bool *died)
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
    if (!p->cwd.assign(cwd.empty() ? vfs_cwd() : cwd) || !p->env.assign(env)) {
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

    // The first step is _start and carries argv then the environment; every one
    // after it is _resume, carrying the answer to a syscall.
    String payload;
    usize n = argv_size(args.v.data(), args.size());
    if (!payload.reserve(n + p->env.size()))
        co_return 1;
    for (usize i = 0; i < n; i++)
        payload.push(0);
    argv_encode(args.v.data(), args.size(), reinterpret_cast<u8 *>(payload.data()));
    if (!payload.append(p->env.str()))
        co_return 1;

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
        CO_RETRY(r, p->done.recv());
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

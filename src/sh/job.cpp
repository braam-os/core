#include "job.h"

#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/traits.h"
#include "kernel/vec.h"
#include "parse.h"

namespace {

// One stage, once its descriptors are decided. A number below SYS_FD_MIN is
// "the stream this shell was given"; anything else is a descriptor of ours,
// which a spawn *moves* out of our table and a builtin only borrows.
struct Stage {
    u32 in  = SYS_STDIN;
    u32 out = SYS_STDOUT;
    u32 err = SYS_STDERR;

    const Builtin *b = nullptr;
    u32 pid          = 0;
    i32 status       = 0;
    bool moved       = false; // its three fds went into a child
};

// Everything a running pipeline owns, on the heap rather than in run_line's
// frame: the allocator's top size class is 512 bytes and a frame past it costs
// a whole 64 KiB span (Concept.md §8.2), and a Pipeline alone is most of one.
struct Run {
    Pipeline pl;
    Vec<Stage> stages;
    Vec<u32> spare; // descriptors nobody took, closed on the way out
};

// One line of the job table. It outlives the frame that started the job, so the
// command text is copied rather than viewed.
struct JobEntry {
    u32 id = 0;
    String cmd;
    Vec<u32> pids;
    bool running = true;
    i32 status   = 0;
};

// Built on first use: a namespace-scope Vec has a destructor, and nothing
// provides __cxa_atexit — the rule holds inside a process exactly as it does in
// the kernel.
Vec<JobEntry *> *g_jobs;
u32 g_next_id = 1;
u32 g_current = 0;

Vec<JobEntry *> &jobs()
{
    if (!g_jobs) {
        g_jobs = heap_new<Vec<JobEntry *>>();
        if (!g_jobs)
            panic("sh: out of memory");
    }
    return *g_jobs;
}

JobEntry *find_entry(u32 id)
{
    for (JobEntry *e : jobs())
        if (e->id == id)
            return e;
    return nullptr;
}

void drop_entry(u32 id)
{
    Vec<JobEntry *> &t = jobs();
    for (usize i = 0; i < t.size(); i++) {
        if (t[i]->id != id)
            continue;
        if (g_current == id)
            g_current = 0;
        heap_delete(t[i]);
        t.erase(i);
        return;
    }
}

// A diagnostic on the shell's own stderr.
Task<void> say(u32 fd, Str what)
{
    String line;
    if (line.assign("braam: ") && line.append(what) && line.push('\n'))
        if (Task<Result<void>> t = write_all(fd, line.str()))
            co_await t;
}

Task<void> say2(u32 fd, Str what, Str why)
{
    String line;
    if (line.assign("braam: ") && line.append(what) && line.append(": ") && line.append(why) &&
        line.push('\n'))
        if (Task<Result<void>> t = write_all(fd, line.str()))
            co_await t;
}

u32 open_flags(Redir kind)
{
    switch (kind) {
    case Redir::In:
        return SYS_O_READ;
    case Redir::Out:
    case Redir::ErrOut:
        return SYS_O_WRITE | SYS_O_CREATE | SYS_O_TRUNC;
    case Redir::Append:
    case Redir::ErrAppend:
        return SYS_O_WRITE | SYS_O_CREATE | SYS_O_APPEND;
    }
    return SYS_O_READ;
}

// Whether a stage's descriptor is one of ours to close rather than one of the
// three we were handed.
bool ours(u32 fd)
{
    return fd >= SYS_FD_MIN;
}

Task<void> close_ours(u32 fd)
{
    if (ours(fd))
        co_await close_fd(fd);
}

// Every descriptor a stage holds that no child took. A builtin's are closed the
// moment it finishes, so that whatever reads the other end sees an end of
// input; a stage that never started leaves its to the sweep at the end.
Task<void> close_stage(const Stage &s)
{
    co_await close_ours(s.in);
    if (s.out != s.in)
        co_await close_ours(s.out);
    if (s.err != s.in && s.err != s.out)
        co_await close_ours(s.err);
}

// Taking the keyboard back, which can in principle be refused. A child's claim
// is the kernel's, on its process record, and that record outlives the child by
// as long as its last syscall server takes to unwind — so a program killed
// while holding the keys may in theory still hold them when the wait for it
// returns.
//
// It has not been seen to happen: the wait's own reply costs a step or two, and
// the servers unwind inside that. But nothing orders the two, and the cost of
// being wrong is a terminal that is simply dead — the next key_read is
// Err(Perm), the prompt never comes back, and there is no way left to say so.
// Eight tries at zero delay is a cheap thing to be sure about.
Task<Result<void>> retake_keys()
{
    for (usize i = 0; i < 8; i++) {
        if (Task<Result<Geometry>> t = keys_claim(true))
            if ((co_await t).is_ok())
                co_return {};
        if (Task<Result<void>> s = sleep_for(0))
            co_await s;
    }
    co_return Err(Error::Perm);
}

// Is this pid still a task the kernel knows about? /proc is how a process asks,
// and asking is the whole point: nothing here may park on a job that is still
// running, because the prompt has to come back either way.
Task<bool> alive(u32 pid)
{
    Buf<32> path;
    path.put("/proc/").put(pid);
    Task<Result<FileInfo>> t = stat_of(path.str());
    if (!t)
        co_return true;
    co_return (co_await t).is_ok();
}

} // namespace

usize jobs_count()
{
    return jobs().size();
}

bool jobs_at(usize i, JobInfo &out)
{
    Vec<JobEntry *> &t = jobs();
    if (i >= t.size())
        return false;
    const JobEntry *e = t[i];
    out = JobInfo{ e->id, e->pids.empty() ? 0 : e->pids[0], e->cmd.str(), e->running, e->status };
    return true;
}

bool jobs_find(u32 id, JobInfo &out)
{
    const JobEntry *e = find_entry(id);
    if (!e)
        return false;
    out = JobInfo{ e->id, e->pids.empty() ? 0 : e->pids[0], e->cmd.str(), e->running, e->status };
    return true;
}

u32 jobs_current()
{
    return g_current;
}

Task<Result<void>> jobs_kill(u32 id)
{
    JobEntry *e = find_entry(id);
    if (!e)
        co_return Err(Error::Invalid);
    if (e->running)
        for (u32 pid : e->pids)
            if (Task<Result<void>> t = kill_child(pid))
                co_await t;
    co_return {};
}

Task<Result<i32>> jobs_wait(u32 id, bool interactive)
{
    JobEntry *e = find_entry(id);
    if (!e)
        co_return Err(Error::Invalid);
    if (!e->running) {
        i32 s = e->status;
        drop_entry(id);
        co_return s;
    }

    // In front while we wait for it, so ^C reaches the job rather than us —
    // which is what `fg` means. The keyboard goes first, in run_line's order
    // and for its reason: a job that wants raw keys can only claim them once we
    // have let go.
    if (interactive) {
        if (Task<Result<Geometry>> t = keys_claim(false))
            co_await t;
        for (u32 pid : e->pids)
            if (Task<Result<void>> t = set_fg(pid))
                co_await t;
    }

    i32 status = 0;
    for (usize i = 0; i < e->pids.size(); i++) {
        Task<Result<Exited>> t = wait_child(e->pids[i]);
        if (!t)
            continue;
        Result<Exited> r = co_await t;
        if (r.is_ok() && i + 1 == e->pids.size())
            status = r.value().status;
    }

    if (interactive) {
        if (Task<Result<void>> t = set_fg(0))
            co_await t;
        if (Task<Result<void>> t = retake_keys())
            co_await t;
    }

    drop_entry(id);
    co_return status;
}

Task<void> jobs_report(u32 fd)
{
    Vec<JobEntry *> &t = jobs();
    for (usize i = 0; i < t.size();) {
        JobEntry *e = t[i];

        if (e->running) {
            bool any = false;
            for (u32 pid : e->pids)
                if (co_await alive(pid))
                    any = true;
            if (any) {
                i++;
                continue;
            }

            // Every stage has gone, so each wait answers at once and none of
            // them parks. Collecting is what frees the kernel's child slots.
            for (usize k = 0; k < e->pids.size(); k++)
                if (Task<Result<Exited>> w = wait_child(e->pids[k])) {
                    Result<Exited> r = co_await w;
                    if (r.is_ok() && k + 1 == e->pids.size())
                        e->status = r.value().status;
                }
            e->running = false;
        }

        Buf<96> line;
        line.put('[').put(e->id).put("] ");
        if (e->status == 0)
            line.put("done      ");
        else if (e->status == 130)
            line.put("interrupt ");
        else
            line.put("exit ").put(e->status).put("    ");
        line.put(e->cmd.str()).put('\n');

        u32 id = e->id;
        if (Task<Result<void>> w = write_all(fd, line.str()))
            co_await w;
        drop_entry(id);
    }
}

Task<i32> run_line(Str line, bool interactive)
{
    Run *r = heap_new<Run>();
    if (!r) {
        co_await say(SYS_STDERR, "out of memory");
        co_return 1;
    }
    struct Drop {
        ~Drop() { heap_delete(r); }

        Run *r;
    } drop{ r };

    Str message;
    if (parse(line, r->pl, message).is_err()) {
        co_await say(SYS_STDERR, message);
        co_return 2;
    }
    usize n = r->pl.size();
    if (n == 0)
        co_return 0;
    if (!r->stages.reserve(n)) {
        co_await say(SYS_STDERR, "out of memory");
        co_return 1;
    }
    for (usize i = 0; i < n; i++)
        r->stages.push(Stage{});

    // Redirections first. Refusing here rather than at the first write is what
    // stops a command running and producing side effects before its
    // redirection turns out to be impossible.
    i32 bad = 0;
    for (usize i = 0; i < n && !bad; i++) {
        for (const Redirect &rd : r->pl.redirects(i)) {
            Str path            = r->pl.target(rd);
            Task<Result<i32>> t = open_at(path, open_flags(rd.kind));
            Result<i32> fd      = t ? co_await t : Err(Error::NoMemory);
            if (fd.is_err()) {
                co_await say2(SYS_STDERR, path, error_name(fd.error()));
                bad = 1;
                break;
            }

            // A second redirection of the same stream replaces the first, which
            // is what `> a > b` means; the earlier file is closed first.
            u32 *slot = rd.kind == Redir::In ? &r->stages[i].in
                        : (rd.kind == Redir::ErrOut || rd.kind == Redir::ErrAppend)
                            ? &r->stages[i].err
                            : &r->stages[i].out;
            if (ours(*slot))
                co_await close_fd(*slot);
            *slot = u32(fd.value());
        }
    }

    // The pipes. Each end is moved into exactly one child, or borrowed by one
    // builtin and closed here — one end, one owner (Concept.md §4.3).
    for (usize i = 0; i + 1 < n && !bad; i++) {
        Task<Result<Piped>> t = make_pipe();
        Result<Piped> p       = t ? co_await t : Err(Error::NoMemory);
        if (p.is_err()) {
            co_await say(SYS_STDERR, "out of memory");
            bad = 1;
            break;
        }
        // A redirection displaces the pipe for that stream, but the pipe is
        // still made and still closed — so `a > f | b` gives b a clean end of
        // input rather than a wait that never finishes.
        if (r->stages[i].out == SYS_STDOUT)
            r->stages[i].out = u32(p.value().w);
        else if (!r->spare.push(u32(p.value().w)))
            bad = 1;
        if (r->stages[i + 1].in == SYS_STDIN)
            r->stages[i + 1].in = u32(p.value().r);
        else if (!r->spare.push(u32(p.value().r)))
            bad = 1;
    }

    for (usize i = 0; i < n; i++)
        r->stages[i].b = builtin_find(r->pl.args(i)[0]);

    // The keyboard goes back *before* anything is spawned. A child is a
    // scheduler job that runs as soon as this shell next parks, and a
    // full-screen program claims the keys in its very first step — so handing
    // them over afterwards is a race the child loses, and `less: no keyboard`
    // is what that looks like.
    bool bg     = r->pl.background();
    bool handed = false;
    if (!bad && !bg && interactive) {
        if (Task<Result<Geometry>> t = keys_claim(false))
            co_await t;
        handed = true;
    }

    // The program stages, all of them before anything runs: a builtin below
    // may write into a pipe whose reader is one of these.
    for (usize i = 0; i < n && !bad; i++) {
        Stage &s = r->stages[i];
        if (s.b)
            continue;
        Task<Result<u32>> t = spawn(r->pl.args(i), ChildIo{ s.in, s.out, s.err });
        Result<u32> pid     = t ? co_await t : Err(Error::NoMemory);
        if (pid.is_err()) {
            co_await say2(SYS_STDERR, r->pl.args(i)[0],
                          pid.error() == Error::NotFound ? "not found" : "not executable");
            bad = pid.error() == Error::NotFound ? 127 : 126;
            break;
        }
        s.pid   = pid.value();
        s.moved = true; // the three descriptors are the child's now
    }

    // In front once they exist, so ^C reaches them rather than us. Nothing is
    // in front between the release above and here, which is the one window in
    // which a ^C is dropped rather than delivered — a few steps wide.
    if (handed)
        for (const Stage &s : r->stages)
            if (s.pid)
                if (Task<Result<void>> t = set_fg(s.pid))
                    co_await t;

    // The builtins, in pipeline order, each to completion. Nothing in this
    // process can wait for a sibling task, so they run in turn rather than
    // alongside — which is why a builtin writes its output once (builtin.h).
    i32 status = bad;
    for (usize i = 0; i < n && !bad; i++) {
        Stage &s = r->stages[i];
        if (!s.b)
            continue;
        if (Task<i32> t = s.b->run(r->pl.args(i), ShIo{ s.in, s.out, s.err }))
            s.status = co_await t;
        else
            s.status = 1;
        // Closed the moment it is done, so whatever reads the other end sees
        // an end of input rather than waiting for a writer that has finished.
        co_await close_stage(s);
        s.in = s.out = s.err = SYS_STDIN;
        if (i + 1 == n)
            status = s.status;
    }

    // Collecting every child. A cancelled one answers 130 of its own accord,
    // so ^C needs nothing here: it went to the stages, not to us.
    for (usize i = 0; i < n && !bg; i++) {
        Stage &s = r->stages[i];
        if (!s.pid)
            continue;
        Task<Result<Exited>> t = wait_child(s.pid);
        Result<Exited> e       = t ? co_await t : Err(Error::NoMemory);
        s.status               = e.is_ok() ? e.value().status : 1;
        if (i + 1 == n)
            status = s.status;
    }

    if (handed) {
        if (Task<Result<void>> t = set_fg(0))
            co_await t;
        if (Task<Result<void>> t = retake_keys())
            co_await t;
    }

    // Backgrounded: the job goes in the table and the shell comes straight back
    // to the prompt. Its stdin is at end of input from the start, since the
    // keyboard belongs to whatever is in front.
    if (bg && !bad) {
        JobEntry *e = heap_new<JobEntry>();
        if (e && e->cmd.assign(line) && jobs().push(e)) {
            e->id = g_next_id++;
            for (const Stage &s : r->stages)
                if (s.pid)
                    e->pids.push(s.pid);
            g_current = e->id;

            Buf<32> note;
            note.put('[').put(e->id).put("] ").put(e->pids.empty() ? 0 : e->pids[0]).put('\n');
            if (Task<Result<void>> t = write_all(SYS_STDERR, note.str()))
                co_await t;
        } else {
            heap_delete(e);
            co_await say(SYS_STDERR, "out of memory");
            status = 1;
        }
    }

    // Whatever nobody took. A stage that was spawned owns its three and must
    // not have them closed under it.
    for (const Stage &s : r->stages)
        if (!s.moved)
            co_await close_stage(s);
    for (u32 fd : r->spare)
        co_await close_fd(fd);

    co_return status;
}

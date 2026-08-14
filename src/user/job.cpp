#include "job.h"

#include "exec.h"
#include "fs/vfs.h"
#include "io.h"
#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/key.h"
#include "kernel/screen.h"
#include "kernel/text.h"
#include "kernel/traits.h"
#include "kernel/vec.h"
#include "parse.h"
#include "tty.h"

namespace {

constexpr u8 PUMP = 0xFF; // the pump's slot in a report

struct Report {
    i32 status;
    u8 index;
};

// One stage's redirections, already open. Three slots because a command can
// redirect each of its three streams exactly once; a later redirection of the
// same stream replaces the earlier one, as a shell does.
struct Files {
    FileIo in, out, err;
};

// Everything a running pipeline owns. It is a heap block rather than a set of
// locals in the shell's frame for two reasons: the allocator's top size class
// is 512 bytes and a coroutine frame past it costs a whole 64 KiB span, and
// ~Sched destroys jobs in spawn order, so a stage frame must not point at
// anything the shell's frame owns. Refcounted, so the last one out frees it
// whatever order they leave in.
struct Job {
    u32 refs = 1;
    Pipeline pl;
    Pipe input; // the tty pump's end of stage 0's stdin
    Vec<Pipe *> pipes;
    Vec<Files *> files;       // one per stage, index-aligned with the pipeline
    Channel<Report, 16> done; // MAX_STAGES plus the pump, rounded up
    Vec<u32> pids;
    Vec<Executable *> execs; // one per stage; a tier-2 stage reads its own back
    u32 pump_pid = 0;

    ~Job()
    {
        for (Pipe *p : pipes) {
            p->~Pipe();
            heap_free(p);
        }
        for (Files *f : files)
            heap_delete(f);
        for (Executable *e : execs)
            heap_delete(e);
    }
};

void job_release(Job *j)
{
    if (--j->refs == 0) {
        j->~Job();
        heap_free(j);
    }
}

// One line of the job table. It outlives the shell frame that started the job,
// so the command text is copied rather than viewed.
struct JobEntry {
    u32 id  = 0;
    u32 pid = 0;
    String cmd;
    Job *job     = nullptr; // holds a reference of its own
    bool running = true;
    i32 status   = 0;
    u32 wait     = 0; // fg's wake token, while one is parked
};

struct Table {
    Vec<JobEntry *> jobs;
    u32 next_id = 1;
    u32 current = 0;
};

// A pointer built on first use: a namespace-scope Vec has a destructor, and
// nothing provides __cxa_atexit (CLAUDE.md).
Table *g_table = nullptr;

Table &table()
{
    if (!g_table) {
        g_table = static_cast<Table *>(heap_alloc(sizeof(Table)));
        if (!g_table)
            panic("jobs: out of memory");
        new (g_table) Table();
    }
    return *g_table;
}

JobEntry *find_entry(u32 id)
{
    for (JobEntry *e : table().jobs)
        if (e->id == id)
            return e;
    return nullptr;
}

// The entry holds a reference of its own — `fg` and `kill` reach the job
// through it long after run_line's frame is gone.
void drop_entry(u32 id)
{
    Table &t = table();
    for (usize i = 0; i < t.jobs.size(); i++) {
        if (t.jobs[i]->id != id)
            continue;
        if (t.current == id)
            t.current = 0;
        if (t.jobs[i]->job)
            job_release(t.jobs[i]->job);
        heap_delete(t.jobs[i]);
        t.jobs.erase(i);
        return;
    }
}

struct JobRef {
    explicit JobRef(Job *p) : j(p) {}

    JobRef(const JobRef &)            = delete;
    JobRef &operator=(const JobRef &) = delete;

    ~JobRef() { job_release(j); }

    Job *j;
};

// A stage's epilogue. It is a destructor rather than straight-line code so
// that it also runs when the stage is cancelled, and when its frame is
// destroyed while suspended.
struct StageEnd {
    ~StageEnd()
    {
        if (out)
            out->close(); // downstream reads EOF
        if (in)
            in->hangup(); // upstream stops writing
        j->done.try_send(Report{ *status, index });
        job_release(j);
    }

    Job *j;
    Pipe *in;
    Pipe *out;
    i32 *status;
    u8 index;
};

// Opens one redirection into the stage's slot for that stream. A second
// redirection of the same stream replaces the first, which is what `> a > b`
// means; the earlier file is closed first, so the open-file table does not see
// two writers on one path (Concept.md §5.2).
Task<Result<void>> open_redirect(Files &f, Redir kind, Str path)
{
    u32 flags    = 0;
    FileIo *slot = nullptr;
    switch (kind) {
    case Redir::In:
        flags = O_READ;
        slot  = &f.in;
        break;
    case Redir::Out:
        flags = O_WRITE | O_CREATE | O_TRUNC;
        slot  = &f.out;
        break;
    case Redir::Append:
        flags = O_WRITE | O_CREATE | O_APPEND;
        slot  = &f.out;
        break;
    case Redir::ErrOut:
        flags = O_WRITE | O_CREATE | O_TRUNC;
        slot  = &f.err;
        break;
    case Redir::ErrAppend:
        flags = O_WRITE | O_CREATE | O_APPEND;
        slot  = &f.err;
        break;
    }
    slot->reset();

    Task<Result<i32>> t = vfs_open(path, flags);
    if (!t)
        co_return Err(Error::NoMemory);
    i32 fd = CO_TRY(co_await t);

    // Appending is a starting offset, not a mode: nothing below the VFS is
    // seekable, so the position is this side's to keep.
    u64 at = 0;
    if (flags & O_APPEND) {
        Result<u64> s = vfs_size(fd);
        if (s.is_ok())
            at = s.value();
    }
    slot->fd  = fd;
    slot->off = at;
    co_return {};
}

// The one place a program is entered, at either tier. The body arrives already
// built, because a tier-2 stage needs its pid — which sched_spawn only hands
// back once this task exists — written into the Executable first.
Task<i32> stage(Job *j, u8 index, Task<i32> t, Pipe *in, Pipe *out)
{
    i32 status = 1;
    StageEnd end{ j, in, out, &status, index };

    if (!t) // the frame would not allocate
        co_return status;
    status = co_await t;
    co_return status;
}

struct PumpEnd {
    ~PumpEnd()
    {
        j->done.try_send(Report{ *status, PUMP });
        job_release(j);
    }

    Job *j;
    i32 *status;
};

// The only receiver on keys() while a pipeline runs, which is what keeps
// Channel's one-receiver rule true: the shell is parked on `done`, not here.
// A full-screen program does not displace it — it claims a route through it
// (tty.h), which is why the two lookups below come before everything else.
//
// It never parks on the input pipe. A pipeline whose head ignores stdin —
// `ls | grep foo` — would otherwise fill the ring after a few keystrokes,
// take the pump off the keyboard, and make ^C unreachable. Dropping is the
// policy key() already uses on the keyboard ring, for the same reason.
Task<i32> tty_pump(Job *j)
{
    i32 status = 0;
    PumpEnd end{ j, &status };
    String line; // the cooked line, sent whole on Enter

    for (;;) {
        Result<Key> r = co_await keys().recv();
        if (r.is_err()) {
            if (r.error() == Error::Again)
                continue; // a stray wake
            co_return status;
        }

        Key k = r.value();

        // ^C reaches the pipeline whatever is claimed: a program that has
        // taken the screen and stopped answering must still be killable, and
        // that is also M4's acceptance criterion.
        if ((k.mods & MOD_CTRL) && k.code == 'c') {
            screen_write("^C");
            screen_newline();
            for (u32 pid : j->pids)
                sched_cancel(pid);
            status = 130;
            co_return status;
        }

        // Raw: no echo and no line discipline, since the claimant is painting
        // the screen itself.
        if (KeyRing *raw = tty_raw()) {
            raw->try_send(k);
            continue;
        }

        // Cooked, to whichever job's stdin is claimed — `fg`'s, or our own.
        Pipe &to = tty_cooked() ? *tty_cooked() : j->input;

        if (k.mods & MOD_CTRL) {
            if (k.code != 'd')
                continue;

            // End of input, not end of the pipeline. A partial line goes
            // first: ^D after typing is "send what I have", as it is anywhere.
            if (!line.empty()) {
                String chunk;
                if (chunk.assign(line.str()))
                    to.try_send(move(chunk));
                line.clear();
            }
            to.close();
            continue;
        }

        // A line at a time, which is what "cooked" means and what the pipe can
        // hold: one chunk per keystroke filled its eight slots after seven
        // characters, and the pump drops rather than parks, so the rest of the
        // line was lost. An applet drained the pipe in the same tick and never
        // showed it; a process reads one syscall at a time and always would.
        // Echo stays per keystroke, so typing still appears as it is typed.
        if (k.code == KEY_ENTER) {
            screen_newline();
            if (line.push('\n')) {
                String chunk;
                if (chunk.assign(line.str()))
                    to.try_send(move(chunk));
            }
            line.clear();
        } else if (k.printable()) {
            char utf8[4];
            usize n = utf8_encode(k.code, utf8);
            line.append(Str(utf8, n));
            screen_put(k.code);
        }
    }
}

// A background job's join loop. It stands where run_line stands for a
// foreground pipeline: nobody is waiting, so somebody has to collect the
// reports, drop the job's last reference and record the status for `jobs`.
Task<i32> reaper(Job *j, JobEntry *e, usize want, u8 last)
{
    i32 status = 0;

    struct End {
        ~End()
        {
            e->running = false;
            e->status  = *status;
            if (e->wait)
                sched_wake(e->wait, 0, 0);
            job_release(j);
        }

        Job *j;
        JobEntry *e;
        i32 *status;
    } end{ j, e, &status };

    for (usize got = 0; got < want;) {
        Result<Report> r = co_await j->done.recv();
        if (r.is_err()) {
            if (r.error() == Error::Again)
                continue;
            for (u32 pid : j->pids)
                sched_cancel(pid);
            co_return 130;
        }
        got++;
        if (r.value().index == last)
            status = r.value().status;
    }
    co_return status;
}

} // namespace

usize jobs_count()
{
    return table().jobs.size();
}

bool jobs_at(usize i, JobInfo &out)
{
    Table &t = table();
    if (i >= t.jobs.size())
        return false;
    const JobEntry *e = t.jobs[i];
    out               = JobInfo{ e->id, e->pid, e->cmd.str(), e->running, e->status };
    return true;
}

bool jobs_find(u32 id, JobInfo &out)
{
    const JobEntry *e = find_entry(id);
    if (!e)
        return false;
    out = JobInfo{ e->id, e->pid, e->cmd.str(), e->running, e->status };
    return true;
}

u32 jobs_current()
{
    return table().current;
}

bool jobs_kill(u32 id)
{
    JobEntry *e = find_entry(id);
    if (!e)
        return false;
    if (e->running && e->job)
        for (u32 pid : e->job->pids)
            sched_cancel(pid);
    return true;
}

Pipe *jobs_input(u32 id)
{
    JobEntry *e = find_entry(id);
    return e && e->job ? &e->job->input : nullptr;
}

Task<Result<i32>> jobs_wait(u32 id)
{
    JobEntry *e = find_entry(id);
    if (!e)
        co_return Err(Error::Invalid);
    if (!e->running) {
        i32 s = e->status;
        drop_entry(id);
        co_return s;
    }

    // The entry cannot be dropped under us: only jobs_report drops one, and it
    // runs in the shell, which is parked on this job's pipeline while we wait.
    Wake w;
    struct Clear {
        ~Clear()
        {
            if (e->wait == token)
                e->wait = 0;
        }

        JobEntry *e;
        u32 token;
    } clear{ e, w.token() };

    e->wait = w.token();
    if (Result<Payload> r = co_await w; r.is_err())
        co_return Err(r.error());

    i32 s = e->status;
    drop_entry(id);
    co_return s;
}

Task<void> jobs_report(Stream out)
{
    Table &t = table();
    for (usize i = 0; i < t.jobs.size();) {
        JobEntry *e = t.jobs[i];
        if (e->running) {
            i++;
            continue;
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
        co_await write_all(out, line.str());
        drop_entry(id);
    }
}

void jobs_reset()
{
    if (!g_table)
        return;
    for (JobEntry *e : g_table->jobs) {
        if (e->job)
            job_release(e->job);
        heap_delete(e);
    }
    g_table->~Table();
    heap_free(g_table);
    g_table = nullptr;
}

Task<i32> run_line(Str line, Stdio io)
{
    Job *j = static_cast<Job *>(heap_alloc(sizeof(Job)));
    if (!j) {
        co_await io.err.write("braam: out of memory\n");
        co_return 1;
    }
    new (j) Job();
    JobRef ref(j);

    Str message;
    if (parse(line, j->pl, message).is_err()) {
        co_await io.err.write("braam: ");
        co_await io.err.write(message);
        co_await io.err.write("\n");
        co_return 2;
    }
    if (j->pl.size() == 0)
        co_return 0;

    usize n = j->pl.size();

    // Every redirection is opened before any stage is spawned. Refusing here
    // rather than at the first write is what stops a command running and
    // producing side effects before its redirection turns out to be
    // impossible, which is what a real shell does.
    for (usize i = 0; i < n; i++) {
        Files *f = heap_new<Files>();
        if (!f || !j->files.push(f)) {
            heap_delete(f);
            co_await io.err.write("braam: out of memory\n");
            co_return 1;
        }
        for (const Redirect &r : j->pl.redirects(i)) {
            Str path        = j->pl.target(r);
            Result<void> ok = co_await open_redirect(*f, r.kind, path);
            if (ok.is_err()) {
                co_await io.err.write("braam: ");
                co_await io.err.write(path);
                co_await io.err.write(": ");
                co_await io.err.write(error_name(ok.error()));
                co_await io.err.write("\n");
                co_return 1;
            }
        }
    }

    // Resolution is where the tier is decided, out of the binary's metadata
    // (Concept.md §4). It reads the image, so it can fail with a diagnostic
    // before anything has run — which is what "not found" already did.
    if (!j->execs.reserve(n)) {
        co_await io.err.write("braam: out of memory\n");
        co_return 1;
    }
    for (usize i = 0; i < n; i++) {
        Args a        = j->pl.args(i);
        Executable *e = heap_new<Executable>();
        if (!e || !j->execs.push(e)) {
            heap_delete(e);
            co_await io.err.write("braam: out of memory\n");
            co_return 1;
        }

        Result<void> r = Err(Error::NoMemory);
        if (Task<Result<void>> t = exec_resolve(a[0], *e))
            r = co_await t;
        if (r.is_err()) {
            co_await io.err.write("braam: ");
            co_await io.err.write(a[0]);
            co_await io.err.write(r.error() == Error::NotFound ? ": not found\n"
                                                               : ": not executable\n");
            co_return r.error() == Error::NotFound ? 127 : 126;
        }
    }

    for (usize i = 0; i + 1 < n; i++) {
        Pipe *p = static_cast<Pipe *>(heap_alloc(sizeof(Pipe)));
        if (!p || !j->pipes.push(p)) {
            heap_free(p);
            co_await io.err.write("braam: out of memory\n");
            co_return 1;
        }
        new (p) Pipe();
    }

    // Cancelling every child from a destructor, not a branch: a cancelled
    // shell cannot park again to clean up, and its frame may be destroyed
    // outright. This covers both.
    struct CancelAll {
        ~CancelAll()
        {
            if (!armed)
                return;
            for (u32 pid : j->pids)
                sched_cancel(pid);
            if (j->pump_pid)
                sched_cancel(j->pump_pid);
        }

        Job *j;
        bool armed = true;
    } cancel_all{ j };

    usize launched = 0;
    for (usize i = 0; i < n; i++) {
        Pipe *in  = i == 0 ? &j->input : j->pipes[i - 1];
        Pipe *out = i + 1 < n ? j->pipes[i] : nullptr;

        // A redirection displaces the pipe for that stream, but the pipe still
        // exists and StageEnd still closes it — so `a > f | b` gives b a clean
        // end of input rather than a wait that never finishes.
        Files *f = j->files[i];

        Stdio sio;
        sio.in  = f->in.fd >= 0 ? file_source(f->in) : pipe_source(*in);
        sio.out = f->out.fd >= 0 ? file_sink(f->out) : (out ? pipe_sink(*out) : io.out);
        sio.err = f->err.fd >= 0 ? file_sink(f->err) : io.err;

        // The name the scheduler keeps is argv[0], a view into the job's word
        // store, which outlives every stage — a binary has no name of its own
        // in the kernel, and the path is not what /proc should show.
        Executable *e = j->execs[i];
        Args a        = j->pl.args(i);
        Task<i32> body = e->builtin ? e->builtin->run(a, sio) : exec_process(*e, a, sio);

        j->refs++; // the stage's own reference, dropped by StageEnd
        u32 pid = sched_spawn(stage(j, u8(i), move(body), in, out), a[0]);
        e->pid  = pid;
        if (!pid || !j->pids.push(pid)) {
            j->refs--;
            if (pid)
                sched_cancel(pid);
            break;
        }
        launched++;
    }

    if (launched < n)
        co_await io.err.write("braam: out of memory\n");

    // Backgrounded: nobody waits, so the job goes in the table with a reaper
    // of its own and the shell comes straight back to the prompt. It gets no
    // pump — the keyboard belongs to whatever runs in the foreground — and its
    // stdin is therefore at end of input from the start.
    if (j->pl.background() && launched == n) {
        j->input.close();

        JobEntry *e = heap_new<JobEntry>();
        Table &t    = table();
        if (e && e->cmd.assign(line) && t.jobs.push(e)) {
            e->id  = t.next_id++;
            e->pid = j->pids[0];
            e->job = j;
            j->refs++; // the entry's, dropped by drop_entry
            t.current = e->id;

            j->refs++; // the reaper's, dropped by its End
            if (sched_spawn(reaper(j, e, launched, u8(n - 1)), "reap")) {
                cancel_all.armed = false;

                Buf<32> note;
                note.put('[').put(e->id).put("] ").put(e->pid).put('\n');
                co_await io.err.write(note.str());
                co_return 0;
            }
            j->refs -= 2;
            t.jobs.pop();
        }
        heap_delete(e);
        co_await io.err.write("braam: out of memory\n");
        co_return 1;
    }

    j->refs++;
    j->pump_pid = sched_spawn(tty_pump(j), "tty");
    if (!j->pump_pid)
        j->refs--;

    if (launched == 0 && j->pump_pid)
        sched_cancel(j->pump_pid);

    usize want       = launched + (j->pump_pid ? 1 : 0);
    usize left       = launched;
    bool pump_in     = j->pump_pid == 0;
    bool interrupted = false;
    i32 status       = launched < n ? 1 : 0;

    for (usize got = 0; got < want;) {
        Result<Report> r = co_await j->done.recv();
        if (r.is_err()) {
            if (r.error() == Error::Again)
                continue;
            co_return 130; // the shell itself is going away; CancelAll cleans up
        }
        got++;

        Report d = r.value();
        if (d.index == PUMP) {
            pump_in = true;
            if (d.status == 130)
                interrupted = true;
            continue;
        }
        if (d.index + 1u == n)
            status = d.status;
        // The pump is only ever ended by us, and the shell must not take the
        // keyboard back until its receiver has provably unwound.
        if (--left == 0 && !pump_in)
            sched_cancel(j->pump_pid);
    }

    co_return interrupted ? 130 : status;
}

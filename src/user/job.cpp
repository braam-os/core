#include "job.h"

#include "fs/vfs.h"
#include "io.h"
#include "kernel/alloc.h"
#include "kernel/key.h"
#include "kernel/screen.h"
#include "kernel/text.h"
#include "kernel/traits.h"
#include "kernel/vec.h"
#include "parse.h"

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
    u32 pump_pid = 0;

    ~Job()
    {
        for (Pipe *p : pipes) {
            p->~Pipe();
            heap_free(p);
        }
        for (Files *f : files)
            heap_delete(f);
    }
};

void job_release(Job *j)
{
    if (--j->refs == 0) {
        j->~Job();
        heap_free(j);
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

Task<i32> stage(Job *j, u8 index, const Program *p, Args args, Stdio io, Pipe *in, Pipe *out)
{
    i32 status = 1;
    StageEnd end{ j, in, out, &status, index };

    Task<i32> t = p->run(args, io);
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
//
// It never parks on the input pipe. A pipeline whose head ignores stdin —
// `ls | grep foo` — would otherwise fill the ring after a few keystrokes,
// take the pump off the keyboard, and make ^C unreachable. Dropping is the
// policy key() already uses on the keyboard ring, for the same reason.
Task<i32> tty_pump(Job *j)
{
    i32 status = 0;
    PumpEnd end{ j, &status };

    for (;;) {
        Result<Key> r = co_await keys().recv();
        if (r.is_err()) {
            if (r.error() == Error::Again)
                continue; // a stray wake
            co_return status;
        }

        Key k = r.value();
        if (k.mods & MOD_CTRL) {
            if (k.code == 'c') {
                screen_write("^C");
                screen_newline();
                for (u32 pid : j->pids)
                    sched_cancel(pid);
                status = 130;
                co_return status;
            }
            if (k.code == 'd')
                j->input.close(); // end of input, not end of the pipeline
            continue;
        }

        char utf8[4];
        usize n = 0;
        if (k.code == KEY_ENTER) {
            utf8[n++] = '\n';
            screen_newline();
        } else if (k.printable()) {
            n = utf8_encode(k.code, utf8);
            screen_put(k.code);
        } else {
            continue;
        }

        String chunk;
        if (chunk.assign(Str(utf8, n)))
            j->input.try_send(move(chunk));
    }
}

} // namespace

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

    Vec<const Program *> progs;
    if (!progs.reserve(n)) {
        co_await io.err.write("braam: out of memory\n");
        co_return 1;
    }
    for (usize i = 0; i < n; i++) {
        Args a           = j->pl.args(i);
        const Program *p = program_find(a[0]);
        if (!p) {
            co_await io.err.write("braam: ");
            co_await io.err.write(a[0]);
            co_await io.err.write(": not found\n");
            co_return 127;
        }
        progs.push(p);
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
            for (u32 pid : j->pids)
                sched_cancel(pid);
            if (j->pump_pid)
                sched_cancel(j->pump_pid);
        }

        Job *j;
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

        j->refs++; // the stage's own reference, dropped by StageEnd
        u32 pid = sched_spawn(stage(j, u8(i), progs[i], j->pl.args(i), sio, in, out));
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

    j->refs++;
    j->pump_pid = sched_spawn(tty_pump(j));
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

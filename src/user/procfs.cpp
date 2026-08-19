#include "procfs.h"

#include "boot.h"
#include "console.h"
#include "exec.h"
#include "fs/vfs.h"
#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/sched.h"
#include "kernel/screen.h"
#include "kernel/text.h"
#include "kernel/traits.h"
#include "kernel/version.h"
#include "tty.h"

namespace {

// A snapshot is taken at open() and read out of afterwards, which is what makes
// a read of /proc consistent: the alternative is a file whose second block
// describes a different moment from its first.
constexpr usize PROC_MAX = 64;

constexpr Str FILES[] = {
    "cwd", "host", "meminfo", "mounts", "stat", "tasks", "uptime", "version"
};

// What /proc/tasks and /proc/<pid> share, so the two cannot disagree.
Str state_of(const ProcInfo &p)
{
    return p.cancelled ? Str("cancelled") : p.waiting ? Str("waiting") : Str("ready");
}

Str wait_of(const ProcInfo &p)
{
    switch (p.wait) {
    case ProcInfo::Wait::Timer:
        return "timer";
    case ProcInfo::Wait::Host:
        return "host";
    case ProcInfo::Wait::Park:
        return "park";
    case ProcInfo::Wait::None:
        break;
    }
    return "-";
}

// A worker is exactly a process: nothing, an instance, or one the stepper has
// finished with.
Str worker_of(const ProcState &st)
{
    return st.worker ? Str("bound") : Str("-");
}

// The foreground and the two console claims, which `ps` prints as STAT.
void flags_of(Buf<8> &b, u32 pid)
{
    if (console_fg_has(pid))
        b.put('+');
    if (tty_keys_owner() == pid)
        b.put('k');
    if (tty_screen_owner() == pid)
        b.put('s');
    if (b.str().empty())
        b.put('-');
}

// Elapsed since the spawn. A task spawned before the first tick started at zero,
// so init's age is the uptime.
u64 age_of(const ProcInfo &p)
{
    f64 age = sched_now() - p.started;
    return age > 0 ? u64(age) : 0;
}

// A word field, and the shell quotes: a space would end it, so it becomes one.
bool put_word(String &out, Str s)
{
    if (s.empty())
        return out.push('-');
    for (usize i = 0; i < s.size(); i++)
        if (!out.push(s[i] == ' ' ? '_' : s[i]))
            return false;
    return true;
}

// One task as positional fields, the way /proc/mounts reads: pid, name, state,
// wait, flags, ppid, calls, fds, mem, cap, age, cwd. The cwd is last, so nothing
// after it needs finding; `-` is "no answer here", and a cwd is what a row with
// a process behind it has — /proc/<pid> names the worker.
bool put_task(String &out, const ProcInfo &p)
{
    ProcState st;
    exec_proc_state(p.pid, st);

    Buf<8> flags;
    flags_of(flags, p.pid);

    Buf<32> head;
    head.put(p.pid).put(' ');

    Buf<128> rest;
    rest.put(' ').put(state_of(p)).put(' ').put(wait_of(p));
    rest.put(' ').put(flags.str());
    rest.put(' ').put(st.ppid).put(' ').put(st.calls).put(' ').put(st.fds);
    rest.put(' ').put(u64(st.pages) * PROC_PAGE).put(' ').put(u64(st.max_pages) * PROC_PAGE);
    rest.put(' ').put(age_of(p)).put(' ');

    return out.append(head.str()) && put_word(out, p.name) && out.append(rest.str()) &&
           put_word(out, st.cwd) && out.push('\n');
}

// One `name value` line of /proc/stat, appended per line as /proc/mounts is: a
// Buf for the whole file would truncate it silently.
bool put_counter(String &out, Str name, u64 value)
{
    Buf<32> one; // 8 padded + a space + 20 digits + a newline
    one.put_left(name, 8).put(' ').put(value).put('\n');
    return out.append(one.str());
}

bool generate(Str name, String &out)
{
    out.clear();
    Buf<128> b;

    if (name == "meminfo") {
        HeapStats s = heap_stats();
        b.put("reserved ").put(u64(s.bytes_reserved)).put("\nin_use   ").put(u64(s.bytes_in_use));
        b.put("\nspans    ").put(u64(s.spans)).put("\nallocs   ").put(u64(s.allocs));
        b.put("\nfrees    ").put(u64(s.frees)).put('\n');
        return out.append(b.str());
    }

    // Every counter the kernel keeps, in one open, which is what a rate needs:
    // cumulative, and differenced by whoever reads it twice. `now` is the clock
    // they were counted against. The heap's figures repeat /proc/meminfo's the
    // way /proc/tasks repeats /proc/<pid>.
    if (name == "stat") {
        SchedStats s = sched_stats();
        HeapStats h  = heap_stats();
        ExecStats e;
        exec_stats(e);

        struct Counter {
            Str name;
            u64 value;
        };
        const Counter fields[] = {
            { "now", u64(sched_now()) },
            { "ticks", s.ticks },
            { "resumes", s.resumes },
            { "timers", s.timers },
            { "wakes", s.wakes },
            { "unparks", s.unparks },
            { "misses", s.misses },
            { "spawns", s.spawns },
            { "wraps", s.wraps },
            { "syscalls", e.syscalls },
            { "sysfast", e.sysfast },
            { "steps", e.steps },
            { "in_use", u64(h.bytes_in_use) },
            { "reserved", u64(h.bytes_reserved) },
            { "allocs", u64(h.allocs) },
            { "frees", u64(h.frees) },
            { "frames", u64(h.frames) },
            { "grows", u64(h.grows) },
            { "fails", u64(h.fails) },
            // The four below partition `tasks`.
            { "tasks", u64(s.tasks) },
            { "ready", u64(s.ready) },
            { "on_timer", u64(s.on_timer) },
            { "on_host", u64(s.on_host) },
            { "on_park", u64(s.on_park) },
        };
        for (const Counter &c : fields)
            if (!put_counter(out, c.name, c.value))
                return false;
        return true;
    }

    // The kernel's own working directory (Concept.md §5.1), which is what init
    // runs /bin/sh from. Every process has one of its own, under its pid below,
    // because this file cannot know who is reading it — and the shell's is now
    // one of those rather than this one.
    if (name == "cwd") {
        b.put(vfs_cwd()).put('\n');
        return out.append(b.str());
    }

    if (name == "uptime") {
        b.put(u64(sched_now())).put(" ms\n");
        return out.append(b.str());
    }

    if (name == "version") {
        b.put(BRAAM_VERSION).put('\n');
        return out.append(b.str());
    }

    // What this is and what it runs on, which `uname` reformats. The first four
    // the kernel knows for itself; the rest the host said at boot and boot kept,
    // since a /proc file is generated here and now with nothing to await.
    //
    // The screen is read fresh every time — it moves with the window. Storage is
    // not here at all: quota and usage are live figures, and `df` asks for them.
    if (name == "host") {
        b.put("system   braam\nrelease  ").put(BRAAM_VERSION);
        b.put("\nmachine  wasm32\nscreen   ").put(screen().cols).put('x').put(screen().rows);
        b.put('\n');
        return out.append(b.str()) && out.append(host_facts());
    }

    // prefix, kind, rw|ro, and the bytes the mount holds — which is what `df`
    // needs and only a filesystem holding its own can answer. An OPFS mount
    // says 0: its bytes are part of the origin's usage, not its own.
    if (name == "mounts") {
        for (const Mount &m : vfs_mounts()) {
            Buf<96> one;
            one.put(m.prefix.str()).put(' ').put(m.fs->kind());
            one.put(m.fs->writable() ? Str(" rw ") : Str(" ro "));
            one.put(m.fs->bytes()).put('\n');
            if (!out.append(one.str()))
                return false;
        }
        return true;
    }

    // Every task at once, which is what `ps` reads: one open, one snapshot, so
    // the rows cannot describe different moments.
    if (name == "tasks") {
        ProcInfo procs[PROC_MAX];
        usize n = sched_procs(procs, PROC_MAX);
        for (usize i = 0; i < n; i++)
            if (!put_task(out, procs[i]))
                return false;
        return true;
    }

    // A pid, then: the same facts as one line of /proc/tasks, named.
    Option<u32> pid = parse_u32(name);
    if (!pid)
        return false;

    ProcInfo procs[PROC_MAX];
    usize n = sched_procs(procs, PROC_MAX);
    for (usize i = 0; i < n; i++) {
        if (procs[i].pid != pid.value())
            continue;

        ProcState st;
        exec_proc_state(procs[i].pid, st);

        Buf<8> flags;
        flags_of(flags, procs[i].pid);

        Buf<256> t;
        t.put("pid    ").put(procs[i].pid);
        t.put("\nname   ").put(procs[i].name.empty() ? Str("-") : procs[i].name);
        t.put("\nstate  ").put(state_of(procs[i]));
        t.put("\nwait   ").put(wait_of(procs[i]));
        t.put("\nflags  ").put(flags.str());
        t.put("\nworker ").put(worker_of(st));
        t.put("\nage    ").put(age_of(procs[i])).put(" ms");
        if (st.ppid)
            t.put("\nppid   ").put(st.ppid);

        // The rest is a process's: a job the kernel runs for itself has no
        // descriptors, no cap, and names things with the kernel's authority
        // rather than a directory of its own.
        if (st.worker) {
            t.put("\ncalls  ").put(st.calls);
            t.put("\nfds    ").put(st.fds);
            // What the instance has committed, which only the host can see, and
            // the ceiling it may grow to.
            t.put("\nmem    ").put(u64(st.pages) * PROC_PAGE);
            t.put("\ncap    ").put(u64(st.max_pages) * PROC_PAGE);
            t.put("\ncwd    ");
            // Appended rather than buffered: a path has no length limit.
            return out.append(t.str()) && out.append(st.cwd) && out.push('\n');
        }
        t.put('\n');
        return out.append(t.str());
    }
    return false;
}

bool known(Str name)
{
    for (Str f : FILES)
        if (f == name)
            return true;

    Option<u32> pid = parse_u32(name);
    if (!pid)
        return false;

    ProcInfo procs[PROC_MAX];
    usize n = sched_procs(procs, PROC_MAX);
    for (usize i = 0; i < n; i++)
        if (procs[i].pid == pid.value())
            return true;
    return false;
}

struct ProcFs final : Fs {
    Str kind() const override { return "procfs"; }

    bool writable() const override { return false; }

    Task<Result<Stat>> stat(Str path) override
    {
        if (path == "/")
            co_return Stat{ NodeKind::Dir, 0 };

        Str name = path.substr(1);
        if (!known(name))
            co_return Err(Error::NotFound);

        String text;
        if (!generate(name, text))
            co_return Err(Error::NotFound);
        co_return Stat{ NodeKind::File, text.size() };
    }

    Task<Result<Vec<Entry>>> list(Str path) override
    {
        if (path != "/")
            co_return Err(Error::NotDir);

        Vec<Entry> out;
        for (Str f : FILES) {
            Entry e;
            e.kind = NodeKind::File;
            String text;
            if (generate(f, text))
                e.size = text.size();
            if (!e.name.assign(f) || !out.push(move(e)))
                co_return Err(Error::NoMemory);
        }

        ProcInfo procs[PROC_MAX];
        usize n = sched_procs(procs, PROC_MAX);
        for (usize i = 0; i < n; i++) {
            Buf<16> name;
            name.put(procs[i].pid);

            Entry e;
            e.kind = NodeKind::File;
            String text;
            if (generate(name.str(), text))
                e.size = text.size();
            if (!e.name.assign(name.str()) || !out.push(move(e)))
                co_return Err(Error::NoMemory);
        }
        co_return move(out);
    }

    Task<Result<u32>> open(Str path, u32 flags) override
    {
        if (flags & (O_WRITE | O_CREATE | O_TRUNC | O_APPEND))
            co_return Err(Error::Perm);
        if (path == "/")
            co_return Err(Error::IsDir);

        String text;
        if (!known(path.substr(1)))
            co_return Err(Error::NotFound);
        if (!generate(path.substr(1), text))
            co_return Err(Error::NoMemory);

        for (usize h = 0; h < open_.size(); h++) {
            if (!open_[h]) {
                open_[h] = heap_new<String>(move(text));
                if (!open_[h])
                    co_return Err(Error::NoMemory);
                co_return u32(h);
            }
        }
        String *held = heap_new<String>(move(text));
        if (!held || !open_.push(held)) {
            heap_delete(held);
            co_return Err(Error::NoMemory);
        }
        co_return u32(open_.size() - 1);
    }

    Task<Result<void>> mkdir(Str) override { co_return Err(Error::Perm); }

    Task<Result<void>> remove(Str, bool) override { co_return Err(Error::Perm); }

    Result<usize> read(u32 h, u64 off, u8 *buf, usize n) override
    {
        const String *s = at(h);
        if (!s)
            return Err(Error::Invalid);
        if (off >= s->size())
            return usize(0);
        usize k = min(n, s->size() - usize(off));
        __builtin_memcpy(buf, s->data() + usize(off), k);
        return k;
    }

    Result<usize> write(u32, u64, const u8 *, usize) override { return Err(Error::Perm); }

    Result<u64> size(u32 h) override
    {
        const String *s = at(h);
        if (!s)
            return Err(Error::Invalid);
        return u64(s->size());
    }

    Result<void> truncate(u32, u64) override { return Err(Error::Perm); }

    void close(u32 h) override
    {
        if (h < open_.size()) {
            heap_delete(open_[h]);
            open_[h] = nullptr;
        }
    }

    ~ProcFs() override
    {
        for (String *s : open_)
            heap_delete(s);
    }

private:
    String *at(u32 h) { return h < open_.size() ? open_[h] : nullptr; }

    Vec<String *> open_;
};

} // namespace

Fs *procfs_create()
{
    return heap_new<ProcFs>();
}

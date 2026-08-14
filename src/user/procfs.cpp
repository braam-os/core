#include "procfs.h"

#include "fs/vfs.h"
#include "job.h"
#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/sched.h"
#include "kernel/text.h"
#include "kernel/traits.h"
#include "kernel/version.h"

namespace {

// A snapshot is taken at open() and read out of afterwards, which is what makes
// a read of /proc consistent: the alternative is a file whose second block
// describes a different moment from its first.
constexpr usize PROC_MAX = 64;

constexpr Str FILES[] = { "cwd", "jobs", "meminfo", "mounts", "uptime", "version" };

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

    // The one global working directory (Concept.md §5.1), which is how `pwd`
    // reads it: a process is not isolated in the namespace it can name, so
    // there is one answer and it is text.
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

    if (name == "jobs") {
        JobInfo j;
        for (usize i = 0; jobs_at(i, j); i++) {
            Buf<96> one;
            one.put(j.id).put(' ').put(j.pid);
            one.put(j.running ? Str(" running ") : Str(" done "));
            one.put(j.status).put(' ').put(j.cmd).put('\n');
            if (!out.append(one.str()))
                return false;
        }
        return true;
    }

    // A pid, then: everything the scheduler knows about one task.
    Option<u32> pid = parse_u32(name);
    if (!pid)
        return false;

    ProcInfo procs[PROC_MAX];
    usize n = sched_procs(procs, PROC_MAX);
    for (usize i = 0; i < n; i++) {
        if (procs[i].pid != pid.value())
            continue;
        b.put("pid   ").put(procs[i].pid);
        b.put("\nname  ").put(procs[i].name.empty() ? Str("-") : procs[i].name);
        Str state = procs[i].cancelled ? Str("cancelled")
                    : procs[i].waiting ? Str("waiting")
                                       : Str("ready");
        b.put("\nstate ").put(state);
        b.put('\n');
        return out.append(b.str());
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

#include "binfs.h"

#include "kernel/alloc.h"
#include "kernel/traits.h"
#include "kernel/vec.h"
#include "prog.h"

namespace {

// One line, so that `cat /bin/echo` says something rather than nothing.
usize content(const Program *p, char *out, usize cap)
{
    usize n = 0;
    for (usize i = 0; i < p->usage.size() && n < cap; i++)
        out[n++] = p->usage[i];
    if (n < cap)
        out[n++] = '\n';
    return n;
}

usize content_size(const Program *p)
{
    return p->usage.size() + 1;
}

struct BinFs final : Fs {
    Str kind() const override { return "binfs"; }

    bool writable() const override { return false; }

    Task<Result<Stat>> stat(Str path) override
    {
        if (path == "/")
            co_return Stat{ NodeKind::Dir, 0 };
        const Program *p = program_find(path.substr(1));
        if (!p)
            co_return Err(Error::NotFound);
        co_return Stat{ NodeKind::File, content_size(p) };
    }

    Task<Result<Vec<Entry>>> list(Str path) override
    {
        if (path != "/")
            co_return Err(Error::NotDir);

        Vec<Entry> out;
        for (const Program *p = program_first(); p; p = p->next) {
            Entry e;
            e.kind = NodeKind::File;
            e.size = content_size(p);
            if (!e.name.assign(p->name) || !out.push(move(e)))
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

        const Program *p = program_find(path.substr(1));
        if (!p)
            co_return Err(Error::NotFound);

        for (usize h = 0; h < open_.size(); h++) {
            if (!open_[h]) {
                open_[h] = p;
                co_return u32(h);
            }
        }
        if (!open_.push(p))
            co_return Err(Error::NoMemory);
        co_return u32(open_.size() - 1);
    }

    Task<Result<void>> mkdir(Str) override { co_return Err(Error::Perm); }

    Task<Result<void>> remove(Str, bool) override { co_return Err(Error::Perm); }

    Result<usize> read(u32 h, u64 off, u8 *buf, usize n) override
    {
        const Program *p = at(h);
        if (!p)
            return Err(Error::Invalid);

        char line[128];
        usize len = content(p, line, sizeof(line));
        if (off >= len)
            return usize(0);
        usize k = min(n, len - usize(off));
        __builtin_memcpy(buf, line + usize(off), k);
        return k;
    }

    Result<usize> write(u32, u64, const u8 *, usize) override { return Err(Error::Perm); }

    Result<u64> size(u32 h) override
    {
        const Program *p = at(h);
        if (!p)
            return Err(Error::Invalid);
        return u64(content_size(p));
    }

    Result<void> truncate(u32, u64) override { return Err(Error::Perm); }

    void close(u32 h) override
    {
        if (h < open_.size())
            open_[h] = nullptr;
    }

private:
    const Program *at(u32 h) { return h < open_.size() ? open_[h] : nullptr; }

    Vec<const Program *> open_;
};

} // namespace

Fs *binfs_create()
{
    return heap_new<BinFs>();
}

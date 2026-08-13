#include "vfs.h"

#include "kernel/alloc.h"
#include "kernel/host.h"
#include "kernel/traits.h"
#include "kernel/vec.h"
#include "path.h"

namespace {

struct OpenFile {
    Fs *fs = nullptr;
    u32 h  = 0;
    String path; // absolute, for the single-writer check and for diagnostics
    u32 flags = 0;
    bool used = false;
};

struct Vfs {
    Vec<Mount> mounts;
    Vec<OpenFile> files;
    String cwd;

    ~Vfs()
    {
        for (OpenFile &f : files)
            if (f.used)
                f.fs->close(f.h);
        for (Mount &m : mounts)
            heap_delete(m.fs);
    }
};

Vfs *g = nullptr;

Vfs &vfs()
{
    if (!g) {
        g = heap_new<Vfs>();
        if (!g)
            panic("vfs: out of memory");
        if (!g->cwd.assign("/"))
            panic("vfs: out of memory");
    }
    return *g;
}

OpenFile *file_of(i32 fd)
{
    Vfs &v = vfs();
    if (fd < 0 || usize(fd) >= v.files.size() || !v.files[usize(fd)].used)
        return nullptr;
    return &v.files[usize(fd)];
}

// Case-free byte order, which is all the shell needs.
bool name_less(Str a, Str b)
{
    usize n = min(a.size(), b.size());
    for (usize i = 0; i < n; i++)
        if (a[i] != b[i])
            return u8(a[i]) < u8(b[i]);
    return a.size() < b.size();
}

} // namespace

Result<void> vfs_mount(Str prefix, Fs *fs)
{
    if (!fs)
        return Err(Error::Invalid);

    String abs;
    Result<void> r = path_resolve("/", prefix, abs);
    if (r.is_err()) {
        heap_delete(fs);
        return r;
    }

    Vfs &v = vfs();
    for (Mount &m : v.mounts) {
        if (m.prefix == abs.str()) {
            heap_delete(fs);
            return Err(Error::Exists);
        }
    }

    Mount m;
    m.fs = fs;
    if (!m.prefix.assign(abs.str()) || !v.mounts.push(move(m))) {
        heap_delete(fs);
        return Err(Error::NoMemory);
    }
    return {};
}

Span<const Mount> vfs_mounts()
{
    Vfs &v = vfs();
    return Span<const Mount>(v.mounts.data(), v.mounts.size());
}

// Longest prefix wins, so /home shadows / for everything beneath it.
const Mount *vfs_lookup(Str abs, Str &sub)
{
    Vfs &v            = vfs();
    const Mount *best = nullptr;
    for (const Mount &m : v.mounts)
        if (path_under(m.prefix.str(), abs) && (!best || m.prefix.size() > best->prefix.size()))
            best = &m;
    if (!best)
        return nullptr;

    sub = best->prefix.size() == 1 ? abs : abs.substr(best->prefix.size());
    if (sub.empty())
        sub = "/";
    return best;
}

Str vfs_cwd()
{
    return vfs().cwd.str();
}

Result<void> vfs_abs(Str path, String &out)
{
    return path_resolve(vfs().cwd.str(), path, out);
}

Task<Result<void>> vfs_chdir(Str path)
{
    String abs;
    CO_TRY_VOID(vfs_abs(path, abs));

    Str sub;
    const Mount *m = vfs_lookup(abs.str(), sub);
    if (!m)
        co_return Err(Error::NotFound);

    Task<Result<Stat>> t = m->fs->stat(sub);
    if (!t)
        co_return Err(Error::NoMemory);

    Stat s = CO_TRY(co_await t);
    if (s.kind != NodeKind::Dir)
        co_return Err(Error::NotDir);
    if (!vfs().cwd.assign(abs.str()))
        co_return Err(Error::NoMemory);
    co_return {};
}

Task<Result<Stat>> vfs_stat(Str path)
{
    String abs;
    CO_TRY_VOID(vfs_abs(path, abs));

    Str sub;
    const Mount *m = vfs_lookup(abs.str(), sub);
    if (!m)
        co_return Err(Error::NotFound);

    Task<Result<Stat>> t = m->fs->stat(sub);
    if (!t)
        co_return Err(Error::NoMemory);
    co_return co_await t;
}

Task<Result<Vec<Entry>>> vfs_list(Str path)
{
    String abs;
    CO_TRY_VOID(vfs_abs(path, abs));

    Str sub;
    const Mount *m = vfs_lookup(abs.str(), sub);
    if (!m)
        co_return Err(Error::NotFound);

    Task<Result<Vec<Entry>>> t = m->fs->list(sub);
    if (!t)
        co_return Err(Error::NoMemory);
    Vec<Entry> out = CO_TRY(co_await t);

    // A mount point need not exist in the filesystem underneath it, so the
    // table itself supplies the entry. Without this, `ls /` would not show
    // /home at all.
    for (const Mount &mount : vfs_mounts()) {
        if (mount.prefix.size() == 1 || path_dirname(mount.prefix.str()) != abs.str())
            continue;
        Str name  = path_basename(mount.prefix.str());
        bool seen = false;
        for (const Entry &e : out)
            seen = seen || e.name == name;
        if (seen)
            continue;
        Entry e;
        e.kind = NodeKind::Dir;
        if (!e.name.assign(name) || !out.push(move(e)))
            co_return Err(Error::NoMemory);
    }

    // Insertion sort: a directory listing is small and this needs no scratch.
    for (usize i = 1; i < out.size(); i++)
        for (usize k = i; k > 0 && name_less(out[k].name.str(), out[k - 1].name.str()); k--)
            swap(out[k], out[k - 1]);

    co_return move(out);
}

Task<Result<i32>> vfs_open(Str path, u32 flags)
{
    String abs;
    CO_TRY_VOID(vfs_abs(path, abs));

    Str sub;
    const Mount *m = vfs_lookup(abs.str(), sub);
    if (!m)
        co_return Err(Error::NotFound);
    if ((flags & O_WRITE) && !m->fs->writable())
        co_return Err(Error::Perm);

    // Concept.md §5.2: an OPFS sync access handle takes an exclusive lock on
    // the file, and a second one fails whatever mode it asks for. The table
    // enforces that for every backend, so the rule does not depend on which
    // one a path happens to land in.
    Vfs &v = vfs();
    for (const OpenFile &f : v.files)
        if (f.used && f.path == abs.str())
            co_return Err(Error::Perm);

    Task<Result<u32>> t = m->fs->open(sub, flags);
    if (!t)
        co_return Err(Error::NoMemory);
    u32 h = CO_TRY(co_await t);

    OpenFile f;
    f.fs    = m->fs;
    f.h     = h;
    f.flags = flags;
    f.used  = true;
    if (!f.path.assign(abs.str())) {
        m->fs->close(h);
        co_return Err(Error::NoMemory);
    }

    for (usize i = 0; i < v.files.size(); i++) {
        if (!v.files[i].used) {
            v.files[i] = move(f);
            co_return i32(i);
        }
    }
    if (!v.files.push(move(f))) {
        m->fs->close(h);
        co_return Err(Error::NoMemory);
    }
    co_return i32(v.files.size() - 1);
}

Task<Result<void>> vfs_mkdir(Str path)
{
    String abs;
    CO_TRY_VOID(vfs_abs(path, abs));

    Str sub;
    const Mount *m = vfs_lookup(abs.str(), sub);
    if (!m)
        co_return Err(Error::NotFound);
    if (!m->fs->writable())
        co_return Err(Error::Perm);

    Task<Result<void>> t = m->fs->mkdir(sub);
    if (!t)
        co_return Err(Error::NoMemory);
    co_return co_await t;
}

Task<Result<void>> vfs_remove(Str path, bool all)
{
    String abs;
    CO_TRY_VOID(vfs_abs(path, abs));

    Str sub;
    const Mount *m = vfs_lookup(abs.str(), sub);
    if (!m)
        co_return Err(Error::NotFound);
    if (!m->fs->writable())
        co_return Err(Error::Perm);
    if (m->prefix == abs.str())
        co_return Err(Error::Perm); // a mount point is not the filesystem's to drop

    Task<Result<void>> t = m->fs->remove(sub, all);
    if (!t)
        co_return Err(Error::NoMemory);
    co_return co_await t;
}

Result<usize> vfs_read(i32 fd, u64 off, u8 *buf, usize n)
{
    OpenFile *f = file_of(fd);
    if (!f)
        return Err(Error::Invalid);
    if (!(f->flags & O_READ))
        return Err(Error::Perm);
    return f->fs->read(f->h, off, buf, n);
}

Result<usize> vfs_write(i32 fd, u64 off, const u8 *buf, usize n)
{
    OpenFile *f = file_of(fd);
    if (!f)
        return Err(Error::Invalid);
    if (!(f->flags & O_WRITE))
        return Err(Error::Perm);
    return f->fs->write(f->h, off, buf, n);
}

Result<u64> vfs_size(i32 fd)
{
    OpenFile *f = file_of(fd);
    if (!f)
        return Err(Error::Invalid);
    return f->fs->size(f->h);
}

Result<void> vfs_truncate(i32 fd, u64 n)
{
    OpenFile *f = file_of(fd);
    if (!f)
        return Err(Error::Invalid);
    if (!(f->flags & O_WRITE))
        return Err(Error::Perm);
    return f->fs->truncate(f->h, n);
}

void vfs_close(i32 fd)
{
    OpenFile *f = file_of(fd);
    if (!f)
        return;
    f->fs->close(f->h);
    *f = OpenFile{};
}

void vfs_reset()
{
    heap_delete(g);
    g = nullptr;
}

#include "vfs.h"

#include "kernel/alloc.h"
#include "kernel/host.h"
#include "kernel/traits.h"
#include "kernel/vec.h"
#include "path.h"

namespace {

// What may not share. O_TRUNC is here because sharing skips the backend open,
// which is where a truncation happens.
constexpr u32 O_MUTATE = O_WRITE | O_TRUNC;

// One backend handle, however many descriptors are on it. On the heap, so the
// pointer survives the descriptor table growing.
struct OpenShared {
    Fs *fs = nullptr;
    u32 h  = 0;
    String path;       // absolute, half of the key
    u32 refs      = 0; // descriptors pointing here
    bool mutating = false;
};

// One descriptor. The flags are its own; the handle is shared.
struct OpenFile {
    OpenShared *s = nullptr;
    u32 flags     = 0;
    bool used     = false;
};

// Drops one descriptor, closing the file when it was the last.
void slot_release(OpenFile &f)
{
    OpenShared *s = f.s;
    f             = OpenFile{};
    if (s && --s->refs == 0) {
        s->fs->close(s->h);
        heap_delete(s);
    }
}

struct Vfs {
    Vec<Mount> mounts;
    Vec<OpenFile> files;
    String cwd;

    ~Vfs()
    {
        for (OpenFile &f : files)
            if (f.used)
                slot_release(f);
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

// The record for an already-open file, or null. Keyed on the pair: a mount laid
// over an open path is a different file.
OpenShared *shared_of(Fs *fs, Str abs)
{
    Vfs &v = vfs();
    for (OpenFile &f : v.files)
        if (f.used && f.s->fs == fs && f.s->path == abs)
            return f.s;
    return nullptr;
}

// A descriptor on `s`, taking a reference only if it succeeds.
i32 slot_alloc(OpenShared *s, u32 flags)
{
    Vfs &v = vfs();
    OpenFile f;
    f.s     = s;
    f.flags = flags;
    f.used  = true;

    for (usize i = 0; i < v.files.size(); i++) {
        if (!v.files[i].used) {
            v.files[i] = f;
            s->refs++;
            return i32(i);
        }
    }
    if (!v.files.push(f))
        return -1;
    s->refs++;
    return i32(v.files.size() - 1);
}

// Concept.md §5.2: readers share, a writer has the file to itself. The whole
// policy is this predicate, which is also why `mutating` never needs
// recomputing — a record that has it never gains a second descriptor.
Result<i32> share(OpenShared *s, u32 flags)
{
    if ((flags & O_MUTATE) || s->mutating)
        return Err(Error::Perm);
    i32 fd = slot_alloc(s, flags);
    if (fd < 0)
        return Err(Error::NoMemory);
    return fd;
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

    // Concept.md §5.2: descriptors share one handle rather than asking for a
    // second, which an OPFS sync access handle refuses. No backend is opened
    // twice, so the rule does not depend on which one a path lands in.
    if (OpenShared *s = shared_of(m->fs, abs.str()))
        co_return share(s, flags);

    Task<Result<u32>> t = m->fs->open(sub, flags);
    if (!t)
        co_return Err(Error::NoMemory);
    Result<u32> r = co_await t;

    // That await was a window: another task may have opened the file while it
    // ran, and on OPFS that is exactly why `r` failed. Nothing below suspends,
    // so the loser always sees the winner's record here.
    if (OpenShared *s = shared_of(m->fs, abs.str())) {
        if (r.is_ok())
            m->fs->close(r.value());
        co_return share(s, flags);
    }
    if (r.is_err())
        co_return Err(r.error());

    OpenShared *s = heap_new<OpenShared>();
    if (!s || !s->path.assign(abs.str())) {
        heap_delete(s);
        m->fs->close(r.value());
        co_return Err(Error::NoMemory);
    }
    s->fs       = m->fs;
    s->h        = r.value();
    s->mutating = (flags & O_MUTATE) != 0;

    i32 fd = slot_alloc(s, flags);
    if (fd < 0) {
        heap_delete(s);
        m->fs->close(r.value());
        co_return Err(Error::NoMemory);
    }
    co_return fd;
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

Task<Result<void>> vfs_touch(Str path)
{
    String abs;
    CO_TRY_VOID(vfs_abs(path, abs));

    Str sub;
    const Mount *m = vfs_lookup(abs.str(), sub);
    if (!m)
        co_return Err(Error::NotFound);
    if (!m->fs->writable())
        co_return Err(Error::Perm);

    Task<Result<void>> t = m->fs->touch(sub);
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
    return f->s->fs->read(f->s->h, off, buf, n);
}

Result<usize> vfs_write(i32 fd, u64 off, const u8 *buf, usize n)
{
    OpenFile *f = file_of(fd);
    if (!f)
        return Err(Error::Invalid);
    if (!(f->flags & O_WRITE))
        return Err(Error::Perm);
    return f->s->fs->write(f->s->h, off, buf, n);
}

Result<u64> vfs_size(i32 fd)
{
    OpenFile *f = file_of(fd);
    if (!f)
        return Err(Error::Invalid);
    return f->s->fs->size(f->s->h);
}

Result<void> vfs_truncate(i32 fd, u64 n)
{
    OpenFile *f = file_of(fd);
    if (!f)
        return Err(Error::Invalid);
    if (!(f->flags & O_WRITE))
        return Err(Error::Perm);
    return f->s->fs->truncate(f->s->h, n);
}

void vfs_close(i32 fd)
{
    OpenFile *f = file_of(fd);
    if (!f)
        return;
    slot_release(*f);
}

void vfs_reset()
{
    heap_delete(g);
    g = nullptr;
}

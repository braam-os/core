#include "bundlefs.h"

#include "kernel/alloc.h"
#include "kernel/traits.h"

namespace {

constexpr usize HEADER = 12;
constexpr usize SLOT   = 16;

u32 word(const char *p, usize at)
{
    u32 v;
    __builtin_memcpy(&v, p + at, sizeof(v));
    return v;
}

// The path a lookup uses: no leading slash, so it matches the packed names.
Str relative(Str path)
{
    return path.starts_with("/") ? path.substr(1) : path;
}

} // namespace

Result<BundleFs *> bundlefs_create(String archive)
{
    const char *p = archive.data();
    usize n       = archive.size();
    if (n < HEADER || Str(p, 4) != "BBND" || word(p, 4) != 1)
        return Err(Error::Invalid);

    usize count = word(p, 8);
    if (count > (n - HEADER) / SLOT)
        return Err(Error::Invalid);

    BundleFs *fs = heap_new<BundleFs>();
    if (!fs)
        return Err(Error::NoMemory);
    if (!fs->files_.reserve(count)) {
        heap_delete(fs);
        return Err(Error::NoMemory);
    }

    for (usize i = 0; i < count; i++) {
        usize at        = HEADER + i * SLOT;
        u32 name_off    = word(p, at);
        u32 name_len    = word(p, at + 4);
        u32 data_off    = word(p, at + 8);
        u32 size        = word(p, at + 12);
        bool in_archive = u64(name_off) + name_len <= n && u64(data_off) + size <= n;
        if (!in_archive || name_len == 0) {
            heap_delete(fs);
            return Err(Error::Invalid);
        }
        fs->files_.push(BundleFs::File{ Str(p + name_off, name_len), data_off, size });
    }

    // The names view the archive, so it has to move in after they are built
    // and never be touched again.
    fs->archive_ = move(archive);
    return fs;
}

usize BundleFs::find(Str path) const
{
    Str want = relative(path);
    for (usize i = 0; i < files_.size(); i++)
        if (files_[i].name == want)
            return i;
    return Str::npos;
}

Task<Result<Stat>> BundleFs::stat(Str path)
{
    usize i = find(path);
    if (i != Str::npos)
        co_return Stat{ NodeKind::File, files_[i].len };

    // A directory exists exactly when something is packed beneath it.
    Str want = relative(path);
    if (want.empty())
        co_return Stat{ NodeKind::Dir, 0 };
    for (const File &f : files_)
        if (f.name.starts_with(want) && f.name.size() > want.size() && f.name[want.size()] == '/')
            co_return Stat{ NodeKind::Dir, 0 };
    co_return Err(Error::NotFound);
}

Task<Result<Vec<Entry>>> BundleFs::list(Str path)
{
    Str want = relative(path);
    Vec<Entry> out;
    bool found = want.empty();

    for (const File &f : files_) {
        Str rest = f.name;
        if (!want.empty()) {
            if (!f.name.starts_with(want) || f.name.size() <= want.size() ||
                f.name[want.size()] != '/')
                continue;
            rest = f.name.substr(want.size() + 1);
        }
        found = true;

        usize slash = rest.find('/');
        Str name    = slash == Str::npos ? rest : rest.substr(0, slash);

        bool seen = false;
        for (const Entry &e : out)
            seen = seen || e.name == name;
        if (seen)
            continue;

        Entry e;
        e.kind = slash == Str::npos ? NodeKind::File : NodeKind::Dir;
        e.size = slash == Str::npos ? f.len : 0;
        if (!e.name.assign(name) || !out.push(move(e)))
            co_return Err(Error::NoMemory);
    }

    if (!found)
        co_return Err(Error::NotFound);
    co_return move(out);
}

Task<Result<u32>> BundleFs::open(Str path, u32 flags)
{
    if (flags & (O_WRITE | O_CREATE | O_TRUNC | O_APPEND))
        co_return Err(Error::Perm);

    usize i = find(path);
    if (i == Str::npos)
        co_return Err(Error::NotFound);

    for (usize h = 0; h < open_.size(); h++) {
        if (open_[h] < 0) {
            open_[h] = i32(i);
            co_return u32(h);
        }
    }
    if (!open_.push(i32(i)))
        co_return Err(Error::NoMemory);
    co_return u32(open_.size() - 1);
}

Task<Result<void>> BundleFs::mkdir(Str)
{
    co_return Err(Error::Perm);
}

Task<Result<void>> BundleFs::remove(Str, bool)
{
    co_return Err(Error::Perm);
}

Result<usize> BundleFs::read(u32 h, u64 off, u8 *buf, usize n)
{
    if (h >= open_.size() || open_[h] < 0)
        return Err(Error::Invalid);
    const File &f = files_[usize(open_[h])];
    if (off >= f.len)
        return usize(0);

    usize at = usize(off);
    usize k  = min(n, usize(f.len) - at);
    __builtin_memcpy(buf, archive_.data() + f.off + at, k);
    return k;
}

Result<usize> BundleFs::write(u32, u64, const u8 *, usize)
{
    return Err(Error::Perm);
}

Result<u64> BundleFs::size(u32 h)
{
    if (h >= open_.size() || open_[h] < 0)
        return Err(Error::Invalid);
    return u64(files_[usize(open_[h])].len);
}

Result<void> BundleFs::truncate(u32, u64)
{
    return Err(Error::Perm);
}

void BundleFs::close(u32 h)
{
    if (h < open_.size())
        open_[h] = -1;
}

u64 BundleFs::bytes() const
{
    return archive_.size();
}

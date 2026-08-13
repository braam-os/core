#include "memfs.h"

#include "kernel/alloc.h"
#include "kernel/traits.h"
#include "path.h"

namespace {

// A 32-bit heap cannot address more, so an offset past it is a bad call rather
// than a large file.
Result<usize> narrow(u64 v)
{
    if (v > 0xffffffffull)
        return Err(Error::Invalid);
    return usize(v);
}

} // namespace

MemNode::~MemNode()
{
    for (MemNode *k : kids)
        heap_delete(k);
}

MemFs::MemFs()
{
    root_.kind = NodeKind::Dir;
}

MemFs::~MemFs() = default;

MemNode *MemFs::child(MemNode *dir, Str name)
{
    if (dir->kind != NodeKind::Dir)
        return nullptr;
    for (MemNode *k : dir->kids)
        if (k->name == name)
            return k;
    return nullptr;
}

// Paths arrive resolved, so there is nothing here to normalise.
MemNode *MemFs::walk(Str path)
{
    MemNode *at = &root_;
    Str rest    = path;
    while (!rest.empty()) {
        Str name = rest.split('/', rest);
        if (name.empty())
            continue;
        at = child(at, name);
        if (!at)
            return nullptr;
    }
    return at;
}

Result<MemNode *> MemFs::make(MemNode *dir, Str name, NodeKind kind)
{
    MemNode *n = heap_new<MemNode>();
    if (!n)
        return Err(Error::NoMemory);
    n->kind = kind;
    if (!n->name.assign(name) || !dir->kids.push(n)) {
        heap_delete(n);
        return Err(Error::NoMemory);
    }
    return n;
}

MemNode *MemFs::node_of(u32 h)
{
    if (h >= open_.size() || !open_[h].used)
        return nullptr;
    return open_[h].node;
}

Task<Result<Stat>> MemFs::stat(Str path)
{
    MemNode *n = walk(path);
    if (!n)
        co_return Err(Error::NotFound);
    co_return Stat{ n->kind, n->data.size() };
}

Task<Result<Vec<Entry>>> MemFs::list(Str path)
{
    MemNode *n = walk(path);
    if (!n)
        co_return Err(Error::NotFound);
    if (n->kind != NodeKind::Dir)
        co_return Err(Error::NotDir);

    Vec<Entry> out;
    if (!out.reserve(n->kids.size()))
        co_return Err(Error::NoMemory);
    for (MemNode *k : n->kids) {
        Entry e;
        e.kind = k->kind;
        e.size = k->data.size();
        if (!e.name.assign(k->name.str()) || !out.push(move(e)))
            co_return Err(Error::NoMemory);
    }
    co_return move(out);
}

Task<Result<u32>> MemFs::open(Str path, u32 flags)
{
    MemNode *n = walk(path);
    if (!n) {
        if (!(flags & O_CREATE))
            co_return Err(Error::NotFound);
        MemNode *dir = walk(path_dirname(path));
        if (!dir)
            co_return Err(Error::NotFound);
        if (dir->kind != NodeKind::Dir)
            co_return Err(Error::NotDir);
        Result<MemNode *> r = make(dir, path_basename(path), NodeKind::File);
        if (r.is_err())
            co_return Err(r.error());
        n = r.value();
    } else if (n->kind == NodeKind::Dir) {
        co_return Err(Error::IsDir);
    } else if (flags & O_TRUNC) {
        n->data.clear();
    }

    for (usize i = 0; i < open_.size(); i++) {
        if (!open_[i].used) {
            open_[i] = Open{ n, true };
            co_return u32(i);
        }
    }
    if (!open_.push(Open{ n, true }))
        co_return Err(Error::NoMemory);
    co_return u32(open_.size() - 1);
}

Task<Result<void>> MemFs::mkdir(Str path)
{
    if (walk(path))
        co_return Err(Error::Exists);
    MemNode *dir = walk(path_dirname(path));
    if (!dir)
        co_return Err(Error::NotFound);
    if (dir->kind != NodeKind::Dir)
        co_return Err(Error::NotDir);
    Result<MemNode *> r = make(dir, path_basename(path), NodeKind::Dir);
    if (r.is_err())
        co_return Err(r.error());
    co_return {};
}

Task<Result<void>> MemFs::remove(Str path, bool all)
{
    MemNode *n = walk(path);
    if (!n)
        co_return Err(Error::NotFound);
    if (n == &root_)
        co_return Err(Error::Perm);
    if (n->kind == NodeKind::Dir && !n->kids.empty() && !all)
        co_return Err(Error::NotEmpty);

    // An open handle on a removed node would dangle, and there is no unlink
    // semantics here to make it mean anything.
    for (Open &o : open_)
        if (o.used && o.node == n)
            co_return Err(Error::Perm);

    MemNode *dir = walk(path_dirname(path));
    for (usize i = 0; i < dir->kids.size(); i++) {
        if (dir->kids[i] != n)
            continue;
        dir->kids.erase(i);
        heap_delete(n);
        co_return {};
    }
    co_return Err(Error::NotFound);
}

Result<usize> MemFs::read(u32 h, u64 off, u8 *buf, usize n)
{
    MemNode *f = node_of(h);
    if (!f)
        return Err(Error::Invalid);
    usize at = TRY(narrow(off));
    if (at >= f->data.size())
        return usize(0);
    usize k = min(n, f->data.size() - at);
    __builtin_memcpy(buf, f->data.data() + at, k);
    return k;
}

Result<usize> MemFs::write(u32 h, u64 off, const u8 *buf, usize n)
{
    MemNode *f = node_of(h);
    if (!f)
        return Err(Error::Invalid);
    usize at  = TRY(narrow(off));
    usize end = TRY(narrow(u64(at) + n));

    if (!f->data.reserve(end))
        return Err(Error::NoMemory);
    while (f->data.size() < end)
        f->data.push('\0'); // reserved above, so this cannot fail
    __builtin_memcpy(f->data.data() + at, buf, n);
    return n;
}

Result<u64> MemFs::size(u32 h)
{
    MemNode *f = node_of(h);
    if (!f)
        return Err(Error::Invalid);
    return u64(f->data.size());
}

Result<void> MemFs::truncate(u32 h, u64 n)
{
    MemNode *f = node_of(h);
    if (!f)
        return Err(Error::Invalid);
    usize k = TRY(narrow(n));
    if (k > f->data.size()) {
        if (!f->data.reserve(k))
            return Err(Error::NoMemory);
        while (f->data.size() < k)
            f->data.push('\0');
    } else {
        f->data.truncate(k);
    }
    return {};
}

void MemFs::close(u32 h)
{
    if (h < open_.size())
        open_[h] = Open{};
}

u64 MemFs::bytes() const
{
    // An explicit stack, because a recursive helper on a tree of unknown depth
    // would be a shadow-stack hazard.
    u64 total = 0;
    Vec<const MemNode *> todo;
    if (!todo.push(&root_))
        return 0;
    while (!todo.empty()) {
        const MemNode *n = todo.back();
        todo.pop();
        total += n->data.size();
        for (MemNode *k : n->kids)
            if (!todo.push(k))
                return total;
    }
    return total;
}

#include "fs/vfs.h"
#include "harness.h"
#include "kernel/alloc.h"
#include "kernel/traits.h"

namespace {

bool has(const Vec<Entry> &v, Str name)
{
    for (const Entry &e : v)
        if (e.name == name)
            return true;
    return false;
}

Result<usize> write(i32 fd, u64 off, Str s)
{
    return vfs_write(fd, off, reinterpret_cast<const u8 *>(s.data()), s.size());
}

// The last separator in a path within a mount, which always has one.
usize last_slash(Str path)
{
    usize at = 0;
    for (usize i = 0; i < path.size(); i++)
        if (path[i] == '/')
            at = i;
    return at;
}

Str parent_of(Str path)
{
    usize at = last_slash(path);
    return at == 0 ? Str("/") : path.substr(0, at);
}

Str base_of(Str path)
{
    return path.substr(last_slash(path) + 1);
}

// A writable filesystem in linear memory, and a fixture rather than something
// the kernel ships: it answers without suspending, which is what these cases
// need — run_now panics on a suspension, and OpfsFs awaits the host for
// everything but a read or a write.
//
// Flat, because the suite is: names are whole paths within the mount. A write
// inside a file truncates the tail rather than overwriting in place, which is
// enough for a suite that writes files whole.
struct TempFs final : Fs {
    Str kind() const override { return "tempfs"; }

    bool writable() const override { return true; }

    Task<Result<Stat>> stat(Str path) override
    {
        if (path == "/")
            co_return Stat{ NodeKind::Dir, 0 };
        if (blocked(path))
            co_return Err(Error::NotDir);
        const Node *n = find(path);
        if (!n)
            co_return Err(Error::NotFound);
        co_return Stat{ n->kind, n->data.size(), n->mtime };
    }

    Task<Result<Vec<Entry>>> list(Str path) override
    {
        Vec<Entry> out;
        for (const Node &n : nodes_) {
            if (parent_of(n.name.str()) != path)
                continue;
            Entry e;
            e.kind  = n.kind;
            e.size  = n.data.size();
            e.mtime = n.mtime;
            if (!e.name.assign(base_of(n.name.str())) || !out.push(move(e)))
                co_return Err(Error::NoMemory);
        }
        co_return move(out);
    }

    Task<Result<u32>> open(Str path, u32 flags) override
    {
        Node *n = find(path);
        if (n && n->kind == NodeKind::Dir)
            co_return Err(Error::IsDir);
        if (!n) {
            if (!(flags & O_CREATE))
                co_return Err(Error::NotFound);
            n = make(path, NodeKind::File);
            if (!n)
                co_return Err(Error::NoMemory);
        }
        if (flags & O_TRUNC)
            n->data.clear();

        for (usize h = 0; h < open_.size(); h++) {
            if (open_[h] == 0) {
                open_[h] = n->id;
                co_return u32(h);
            }
        }
        if (!open_.push(n->id))
            co_return Err(Error::NoMemory);
        co_return u32(open_.size() - 1);
    }

    Task<Result<void>> mkdir(Str path) override
    {
        if (find(path))
            co_return Err(Error::Exists);
        if (!make(path, NodeKind::Dir))
            co_return Err(Error::NoMemory);
        co_return Result<void>{};
    }

    Task<Result<void>> remove(Str path, bool all) override
    {
        for (usize i = 0; i < nodes_.size(); i++) {
            if (nodes_[i].name.str() != path)
                continue;
            if (nodes_[i].kind == NodeKind::Dir && !all)
                for (const Node &n : nodes_)
                    if (parent_of(n.name.str()) == path)
                        co_return Err(Error::NotEmpty);
            nodes_.erase(i);
            co_return Result<void>{};
        }
        co_return Err(Error::NotFound);
    }

    Result<usize> read(u32 h, u64 off, u8 *buf, usize n) override
    {
        Node *node = node_of(h);
        if (!node)
            return Err(Error::Invalid);
        if (off >= node->data.size())
            return usize(0);
        usize at = usize(off);
        usize k  = min(n, node->data.size() - at);
        __builtin_memcpy(buf, node->data.data() + at, k);
        return k;
    }

    Result<usize> write(u32 h, u64 off, const u8 *buf, usize n) override
    {
        Node *node = node_of(h);
        if (!node)
            return Err(Error::Invalid);
        node->mtime = ++clock_;
        usize at    = usize(off);
        while (node->data.size() < at)
            if (!node->data.push('\0'))
                return Err(Error::NoMemory);
        node->data.truncate(at);
        if (!node->data.append(Str(reinterpret_cast<const char *>(buf), n)))
            return Err(Error::NoMemory);
        return n;
    }

    Result<u64> size(u32 h) override
    {
        Node *node = node_of(h);
        if (!node)
            return Err(Error::Invalid);
        return u64(node->data.size());
    }

    Result<void> truncate(u32 h, u64 n) override
    {
        Node *node = node_of(h);
        if (!node)
            return Err(Error::Invalid);
        node->mtime = ++clock_;
        node->data.truncate(usize(n));
        return {};
    }

    Task<Result<void>> touch(Str path) override
    {
        Node *n = find(path);
        if (!n)
            co_return Err(Error::NotFound);
        n->mtime = ++clock_;
        co_return Result<void>{};
    }

    void close(u32 h) override
    {
        if (h < open_.size())
            open_[h] = 0;
    }

    Task<Result<void>> symlink(Str target, Str path) override
    {
        if (find(path))
            co_return Err(Error::Exists);
        Node *n = make(path, NodeKind::Link);
        if (!n || !n->data.assign(target))
            co_return Err(Error::NoMemory);
        co_return Result<void>{};
    }

    Task<Result<String>> readlink(Str path) override
    {
        const Node *n = find(path);
        if (!n)
            co_return Err(Error::NotFound);
        if (n->kind != NodeKind::Link)
            co_return Err(Error::Invalid);
        String out;
        if (!out.assign(n->data.str()))
            co_return Err(Error::NoMemory);
        co_return move(out);
    }

private:
    // Named by an id rather than an index, so erasing one does not move the
    // file another descriptor is holding.
    struct Node {
        String name;
        String data;
        NodeKind kind = NodeKind::File;
        u32 id        = 0;
        u64 mtime     = 0;
    };

    Node *find(Str path)
    {
        for (Node &n : nodes_)
            if (n.name.str() == path)
                return &n;
        return nullptr;
    }

    const Node *find(Str path) const { return const_cast<TempFs *>(this)->find(path); }

    // A store walks a component at a time, so a non-directory where a directory
    // had to be is Err(NotDir). This flat map has to say the same, or the VFS's
    // link walk never runs.
    bool blocked(Str path) const
    {
        for (usize i = 1; i < path.size(); i++) {
            if (path[i] != '/')
                continue;
            const Node *n = find(path.substr(0, i));
            if (n && n->kind != NodeKind::Dir)
                return true;
        }
        return false;
    }

    Node *make(Str path, NodeKind kind)
    {
        Node n;
        n.kind  = kind;
        n.id    = ++next_;
        n.mtime = ++clock_;
        if (!n.name.assign(path) || !nodes_.push(move(n)))
            return nullptr;
        return &nodes_[nodes_.size() - 1];
    }

    Node *node_of(u32 h)
    {
        if (h >= open_.size() || open_[h] == 0)
            return nullptr;
        for (Node &n : nodes_)
            if (n.id == open_[h])
                return &n;
        return nullptr;
    }

    Vec<Node> nodes_;
    Vec<u32> open_;
    u32 next_  = 0;
    u64 clock_ = 0;
};

// A filesystem that refuses everything, to prove the VFS checks before it
// asks. /bin and /share are both this shape.
struct ReadOnlyFs final : Fs {
    Str kind() const override { return "rofs"; }

    bool writable() const override { return false; }

    Task<Result<Stat>> stat(Str path) override
    {
        co_return path == "/" ? Result<Stat>(Stat{ NodeKind::Dir, 0 }) : Err(Error::NotFound);
    }

    Task<Result<Vec<Entry>>> list(Str) override { co_return Vec<Entry>(); }

    Task<Result<u32>> open(Str, u32) override { co_return Err(Error::NotFound); }

    Task<Result<void>> mkdir(Str) override { co_return Err(Error::Perm); }

    Task<Result<void>> remove(Str, bool) override { co_return Err(Error::Perm); }

    Result<usize> read(u32, u64, u8 *, usize) override { return Err(Error::Invalid); }

    Result<usize> write(u32, u64, const u8 *, usize) override { return Err(Error::Perm); }

    Result<u64> size(u32) override { return Err(Error::Invalid); }

    Result<void> truncate(u32, u64) override { return Err(Error::Perm); }

    void close(u32) override {}
};

// Counts what reaches the filesystem, which is the whole point of sharing: two
// descriptors on one file are one open() and one close() down here. TempFs would
// answer a second open happily, so only these counters can tell the difference.
// They are out here because vfs_reset() destroys the mount before they are read.
u32 fs_opens = 0, fs_closes = 0;

struct CountingFs final : Fs {
    Str kind() const override { return "countfs"; }

    bool writable() const override { return true; }

    Task<Result<Stat>> stat(Str path) override
    {
        co_return Stat{ path == "/" ? NodeKind::Dir : NodeKind::File, 0 };
    }

    Task<Result<Vec<Entry>>> list(Str) override { co_return Vec<Entry>(); }

    Task<Result<u32>> open(Str, u32) override { co_return u32(fs_opens++); }

    Task<Result<void>> mkdir(Str) override { co_return Err(Error::Perm); }

    Task<Result<void>> remove(Str, bool) override { co_return Err(Error::Perm); }

    Result<usize> read(u32, u64, u8 *, usize) override { return usize(0); }

    Result<usize> write(u32, u64, const u8 *, usize n) override { return n; }

    Result<u64> size(u32) override { return u64(0); }

    Result<void> truncate(u32, u64) override { return {}; }

    void close(u32) override { fs_closes++; }
};

} // namespace

void test_vfs()
{
    test_begin("vfs");

    usize in_use = heap_stats().bytes_in_use;
    vfs_reset();

    CHECK(vfs_mount("/", heap_new<TempFs>()).is_ok());
    CHECK(vfs_mount("/home", heap_new<TempFs>()).is_ok());
    CHECK(vfs_mount("/share", heap_new<ReadOnlyFs>()).is_ok());
    CHECK_EQ(vfs_mounts().size(), 3);

    // Mounting twice on one point is an error, and the rejected filesystem is
    // destroyed rather than leaked: the table takes ownership either way.
    CHECK(vfs_mount("/home", heap_new<TempFs>()).error() == Error::Exists);

    // Longest prefix wins, and the path handed down is relative to the mount.
    Str sub;
    CHECK(vfs_lookup("/home/notes", sub)->prefix == "/home");
    CHECK(sub == "/notes");
    CHECK(vfs_lookup("/home", sub)->prefix == "/home");
    CHECK(sub == "/");
    CHECK(vfs_lookup("/tmp/x", sub)->prefix == "/");
    CHECK(sub == "/tmp/x");
    CHECK(vfs_lookup("/homer", sub)->prefix == "/"); // a component, not a substring

    // The cwd starts at the root, and cd validates what it is given.
    CHECK(vfs_cwd() == "/");
    CHECK(run_now(vfs_chdir("/home")).is_ok());
    CHECK(vfs_cwd() == "/home");
    CHECK(run_now(vfs_chdir("/nowhere")).error() == Error::NotFound);
    CHECK(vfs_cwd() == "/home");

    // A relative path resolves against it, and lands in the right mount.
    i32 fd = run_now(vfs_open("notes", O_WRITE | O_CREATE)).value();
    CHECK(write(fd, 0, "hello").value() == 5);
    vfs_close(fd);
    CHECK(run_now(vfs_stat("/home/notes")).value().size == 5);

    // Concept.md §5.2's exclusive lock: a writer has the file to itself, and
    // nothing else may open it while it does.
    fd = run_now(vfs_open("/home/notes", O_WRITE)).value();
    CHECK(run_now(vfs_open("/home/notes", O_WRITE)).error() == Error::Perm);
    CHECK(run_now(vfs_open("/home/notes", O_READ)).error() == Error::Perm);
    vfs_close(fd);
    fd = run_now(vfs_open("/home/notes", O_WRITE)).value();
    vfs_close(fd);

    // Readers share, so `cat notes notes` works. Each descriptor is its own
    // number with its own flags; the offset lives above the VFS entirely.
    i32 a     = run_now(vfs_open("/home/notes", O_READ)).value();
    i32 b     = run_now(vfs_open("/home/notes", O_READ)).value();
    u8 buf[8] = {};
    CHECK(a != b);
    CHECK(vfs_read(a, 0, buf, 5).value() == 5);
    CHECK(Str(reinterpret_cast<const char *>(buf), 5) == "hello");
    CHECK(vfs_read(b, 1, buf, 4).value() == 4);
    CHECK(Str(reinterpret_cast<const char *>(buf), 4) == "ello");

    // A writer is refused while either of them holds it, and closing one leaves
    // the other usable — which is what makes `cat notes > notes` still refused.
    CHECK(run_now(vfs_open("/home/notes", O_WRITE)).error() == Error::Perm);
    vfs_close(a);
    CHECK(run_now(vfs_open("/home/notes", O_WRITE)).error() == Error::Perm);
    CHECK(vfs_read(b, 0, buf, 5).value() == 5);
    CHECK(vfs_read(a, 0, buf, 5).error() == Error::Invalid);
    vfs_close(b);
    fd = run_now(vfs_open("/home/notes", O_WRITE)).value();
    vfs_close(fd);

    // O_TRUNC excludes even without O_WRITE: sharing skips the backend open,
    // which is where a truncation would have happened.
    a = run_now(vfs_open("/home/notes", O_READ)).value();
    CHECK(run_now(vfs_open("/home/notes", O_READ | O_TRUNC)).error() == Error::Perm);
    vfs_close(a);

    // And what sharing means underneath: one open() and one close() reach the
    // filesystem however many descriptors are on the file. The await window in
    // vfs_open cannot be exercised here — run_now panics on a suspension, and
    // every filesystem mounted in this suite answers synchronously.
    CHECK(vfs_mount("/count", heap_new<CountingFs>()).is_ok());
    a = run_now(vfs_open("/count/f", O_READ)).value();
    b = run_now(vfs_open("/count/f", O_READ)).value();
    CHECK(a != b);
    CHECK_EQ(fs_opens, 1u);
    vfs_close(a);
    CHECK_EQ(fs_closes, 0u);
    CHECK(vfs_read(b, 0, buf, sizeof buf).is_ok());
    vfs_close(b);
    CHECK_EQ(fs_closes, 1u);

    // A refused open never reaches the filesystem at all.
    i32 w = run_now(vfs_open("/count/f", O_WRITE)).value();
    CHECK(run_now(vfs_open("/count/f", O_READ)).error() == Error::Perm);
    CHECK(run_now(vfs_open("/count/f", O_WRITE)).error() == Error::Perm);
    CHECK_EQ(fs_opens, 2u);
    vfs_close(w);
    CHECK_EQ(fs_closes, 2u);

    // A descriptor honours what it was opened for.
    fd = run_now(vfs_open("/home/notes", O_READ)).value();
    CHECK(write(fd, 0, "x").error() == Error::Perm);
    CHECK(vfs_truncate(fd, 0).error() == Error::Perm);
    vfs_close(fd);
    CHECK(vfs_size(fd).error() == Error::Invalid);

    // A read-only mount is refused above the filesystem, not by it.
    CHECK(run_now(vfs_open("/share/x", O_WRITE | O_CREATE)).error() == Error::Perm);
    CHECK(run_now(vfs_mkdir("/share/x")).error() == Error::Perm);
    CHECK(run_now(vfs_remove("/share/x", false)).error() == Error::Perm);

    // A mount point is not the filesystem underneath it to drop.
    CHECK(run_now(vfs_remove("/home", true)).error() == Error::Perm);

    // Listing the root folds in the mount points, which do not exist as
    // directories in the filesystem mounted there — and so have no mtime.
    {
        Vec<Entry> root = move(run_now(vfs_list("/")).value());
        CHECK(has(root, "home"));
        CHECK(has(root, "share"));
        for (const Entry &e : root)
            CHECK(e.kind == NodeKind::Dir);
        for (const Entry &e : root)
            if (e.name == "home" || e.name == "share")
                CHECK_EQ(e.mtime, 0u);
    }

    // An mtime rides stat and list alike, moves when the file is written, and
    // moves again on a touch.
    {
        u64 was = run_now(vfs_stat("/home/notes")).value().mtime;
        CHECK(was > 0);
        fd = run_now(vfs_open("/home/notes", O_WRITE)).value();
        CHECK(write(fd, 0, "hello!").value() == 6);
        vfs_close(fd);

        u64 now = run_now(vfs_stat("/home/notes")).value().mtime;
        CHECK(now > was);

        Vec<Entry> home = move(run_now(vfs_list("/home")).value());
        for (const Entry &e : home)
            if (e.name == "notes")
                CHECK_EQ(e.mtime, now);

        CHECK(run_now(vfs_touch("/home/notes")).is_ok());
        CHECK(run_now(vfs_stat("/home/notes")).value().mtime > now);

        // A filesystem keeping none refuses rather than answering 0.
        CHECK(run_now(vfs_touch("/share/x")).error() == Error::Perm);
        CHECK(run_now(vfs_touch("/count/f")).error() == Error::Unsupported);
    }

    // Sorted, which is what makes `ls` output stable.
    CHECK(run_now(vfs_mkdir("/home/a")).is_ok());
    CHECK(run_now(vfs_mkdir("/home/z")).is_ok());
    {
        Vec<Entry> home = move(run_now(vfs_list("/home")).value());
        CHECK_EQ(home.size(), 3);
        CHECK(home[0].name == "a");
        CHECK(home[1].name == "notes");
        CHECK(home[2].name == "z");
    }

    // ------------------------------------------------------ symbolic links
    {
        CHECK(run_now(vfs_mkdir("/home/d")).is_ok());
        i32 lfd = run_now(vfs_open("/home/d/inner", O_WRITE | O_CREATE)).value();
        CHECK(write(lfd, 0, "deep").value() == 4);
        vfs_close(lfd);

        // Its own kind; stat follows, lstat does not.
        CHECK(run_now(vfs_symlink("/home/notes", "/home/link")).is_ok());
        CHECK(run_now(vfs_stat("/home/link", false)).value().kind == NodeKind::Link);
        CHECK(run_now(vfs_stat("/home/link")).value().kind == NodeKind::File);
        CHECK(run_now(vfs_readlink("/home/link")).value().str() == "/home/notes");
        CHECK(run_now(vfs_readlink("/home/notes")).error() == Error::Invalid);

        // A listing never resolves one.
        {
            Vec<Entry> home = move(run_now(vfs_list("/home")).value());
            for (const Entry &e : home)
                if (e.name == "link")
                    CHECK(e.kind == NodeKind::Link);
        }

        // Reading through a link reads the target.
        lfd = run_now(vfs_open("/home/link", O_READ)).value();
        CHECK(vfs_read(lfd, 0, buf, 6).value() == 6);
        CHECK(Str(reinterpret_cast<const char *>(buf), 6) == "hello!");
        vfs_close(lfd);

        // Mid-path: the store answers NotDir and the walk takes over.
        CHECK(run_now(vfs_symlink("/home/d", "/home/dl")).is_ok());
        CHECK(run_now(vfs_stat("/home/dl/inner")).value().size == 4);
        CHECK(run_now(vfs_list("/home/dl")).value().size() == 1);

        // A relative target reads against the directory holding the link.
        CHECK(run_now(vfs_symlink("notes", "/home/rel")).is_ok());
        CHECK(run_now(vfs_stat("/home/rel")).value().kind == NodeKind::File);

        // Two names for one file are one entry in the open-file table, keyed on
        // the physical path — so a reader through one refuses a writer through
        // the other.
        i32 rd = run_now(vfs_open("/home/notes", O_READ)).value();
        CHECK(run_now(vfs_open("/home/link", O_WRITE)).error() == Error::Perm);
        vfs_close(rd);
        i32 wr = run_now(vfs_open("/home/link", O_WRITE)).value();
        vfs_close(wr);

        // A link may cross a mount: every hop goes back through the table.
        CHECK(run_now(vfs_symlink("/share", "/home/tomount")).is_ok());
        CHECK(run_now(vfs_stat("/home/tomount")).value().kind == NodeKind::Dir);
        CHECK(run_now(vfs_stat("/home/tomount", false)).value().kind == NodeKind::Link);
        CHECK(run_now(vfs_list("/home/tomount")).is_ok());
        // The mount it lands in decides a write, not the one named.
        CHECK(run_now(vfs_mkdir("/home/tomount/x")).error() == Error::Perm);
        CHECK(run_now(vfs_remove("/home/tomount", false)).is_ok());

        // A dangling link stats as a link and resolves to nothing.
        CHECK(run_now(vfs_symlink("/home/nothing", "/home/dangle")).is_ok());
        CHECK(run_now(vfs_stat("/home/dangle", false)).value().kind == NodeKind::Link);
        CHECK(run_now(vfs_stat("/home/dangle")).error() == Error::NotFound);

        // Removing a link leaves what it pointed at.
        CHECK(run_now(vfs_remove("/home/link", false)).is_ok());
        CHECK(run_now(vfs_stat("/home/notes")).is_ok());

        // A cycle is bounded.
        CHECK(run_now(vfs_symlink("/home/y", "/home/x")).is_ok());
        CHECK(run_now(vfs_symlink("/home/x", "/home/y")).is_ok());
        CHECK(run_now(vfs_stat("/home/x")).error() == Error::Loop);

        // Not silently replaced.
        CHECK(run_now(vfs_symlink("/home/notes", "/home/rel")).error() == Error::Exists);

        CHECK(run_now(vfs_remove("/home/x", false)).is_ok());
        CHECK(run_now(vfs_remove("/home/y", false)).is_ok());
        CHECK(run_now(vfs_remove("/home/dangle", false)).is_ok());
        CHECK(run_now(vfs_remove("/home/rel", false)).is_ok());
        CHECK(run_now(vfs_remove("/home/dl", false)).is_ok());
        CHECK(run_now(vfs_remove("/home/d", true)).is_ok());
    }

    // Two descriptors still on one file at reset: ~Vfs drops each reference and
    // the last one closes the handle, once rather than twice.
    (void)run_now(vfs_open("/count/f", O_READ)).value();
    (void)run_now(vfs_open("/count/f", O_READ)).value();
    CHECK_EQ(fs_opens, 3u);

    vfs_reset();
    CHECK_EQ(fs_closes, 3u);
    CHECK_EQ(vfs_mounts().size(), 0);
    CHECK(vfs_cwd() == "/");

    vfs_reset();
    CHECK_EQ(heap_stats().bytes_in_use, in_use);
}

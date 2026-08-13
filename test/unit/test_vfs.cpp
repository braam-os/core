#include "fs/memfs.h"
#include "fs/vfs.h"
#include "harness.h"
#include "kernel/alloc.h"

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

// A filesystem that refuses everything, to prove the VFS checks before it
// asks. /usr and /bin are both this shape.
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

} // namespace

void test_vfs()
{
    test_begin("vfs");

    usize in_use = heap_stats().bytes_in_use;
    vfs_reset();

    CHECK(vfs_mount("/", heap_new<MemFs>()).is_ok());
    CHECK(vfs_mount("/home", heap_new<MemFs>()).is_ok());
    CHECK(vfs_mount("/usr", heap_new<ReadOnlyFs>()).is_ok());
    CHECK_EQ(vfs_mounts().size(), 3);

    // Mounting twice on one point is an error, and the rejected filesystem is
    // destroyed rather than leaked: the table takes ownership either way.
    CHECK(vfs_mount("/home", heap_new<MemFs>()).error() == Error::Exists);

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

    // Concept.md §5.2's exclusive lock: one open handle per file, whatever
    // mode the second one asks for, because that is what OPFS enforces and the
    // rule must not depend on which backend a path lands in.
    fd = run_now(vfs_open("/home/notes", O_WRITE)).value();
    CHECK(run_now(vfs_open("/home/notes", O_WRITE)).error() == Error::Perm);
    CHECK(run_now(vfs_open("/home/notes", O_READ)).error() == Error::Perm);
    vfs_close(fd);
    fd = run_now(vfs_open("/home/notes", O_WRITE)).value();
    vfs_close(fd);

    // A descriptor honours what it was opened for.
    fd = run_now(vfs_open("/home/notes", O_READ)).value();
    CHECK(write(fd, 0, "x").error() == Error::Perm);
    CHECK(vfs_truncate(fd, 0).error() == Error::Perm);
    vfs_close(fd);
    CHECK(vfs_size(fd).error() == Error::Invalid);

    // A read-only mount is refused above the filesystem, not by it.
    CHECK(run_now(vfs_open("/usr/x", O_WRITE | O_CREATE)).error() == Error::Perm);
    CHECK(run_now(vfs_mkdir("/usr/x")).error() == Error::Perm);
    CHECK(run_now(vfs_remove("/usr/x", false)).error() == Error::Perm);

    // A mount point is not the filesystem underneath it to drop.
    CHECK(run_now(vfs_remove("/home", true)).error() == Error::Perm);

    // Listing the root folds in the mount points, which do not exist as
    // directories in the filesystem mounted there.
    {
        Vec<Entry> root = move(run_now(vfs_list("/")).value());
        CHECK(has(root, "home"));
        CHECK(has(root, "usr"));
        for (const Entry &e : root)
            CHECK(e.kind == NodeKind::Dir);
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

    vfs_reset();
    CHECK_EQ(vfs_mounts().size(), 0);
    CHECK(vfs_cwd() == "/");

    vfs_reset();
    CHECK_EQ(heap_stats().bytes_in_use, in_use);
}

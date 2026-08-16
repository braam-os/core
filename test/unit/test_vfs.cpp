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
// descriptors on one file are one open() and one close() down here. MemFs would
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

    CHECK(vfs_mount("/", heap_new<MemFs>()).is_ok());
    CHECK(vfs_mount("/home", heap_new<MemFs>()).is_ok());
    CHECK(vfs_mount("/share", heap_new<ReadOnlyFs>()).is_ok());
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
    // directories in the filesystem mounted there.
    {
        Vec<Entry> root = move(run_now(vfs_list("/")).value());
        CHECK(has(root, "home"));
        CHECK(has(root, "share"));
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

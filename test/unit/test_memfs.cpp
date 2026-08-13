#include "fs/memfs.h"
#include "harness.h"
#include "kernel/alloc.h"

namespace {

Result<u32> open(MemFs &fs, Str path, u32 flags)
{
    return run_now(fs.open(path, flags));
}

// Writes a whole string at the offset, as a file-backed Stream would.
Result<usize> write(MemFs &fs, u32 h, u64 off, Str s)
{
    return fs.write(h, off, reinterpret_cast<const u8 *>(s.data()), s.size());
}

Str read(MemFs &fs, u32 h, u64 off, char *buf, usize cap)
{
    Result<usize> r = fs.read(h, off, reinterpret_cast<u8 *>(buf), cap);
    CHECK(r.is_ok());
    return r.is_ok() ? Str(buf, r.value()) : Str();
}

bool has(const Vec<Entry> &v, Str name, NodeKind kind)
{
    for (const Entry &e : v)
        if (e.name == name && e.kind == kind)
            return true;
    return false;
}

} // namespace

void test_memfs()
{
    test_begin("memfs");

    usize in_use = heap_stats().bytes_in_use;
    {
        MemFs fs;
        char buf[64];

        // The root exists from the start, and it is a directory.
        Result<Stat> s = run_now(fs.stat("/"));
        CHECK(s.is_ok());
        CHECK(s.value().kind == NodeKind::Dir);
        CHECK(run_now(fs.list("/")).value().empty());

        // Opening without O_CREATE is not how a file comes into existence.
        CHECK(open(fs, "/notes", O_READ).error() == Error::NotFound);

        u32 h = open(fs, "/notes", O_WRITE | O_CREATE).value();
        CHECK(write(fs, h, 0, "hello").value() == 5);
        CHECK(fs.size(h).value() == 5);
        CHECK(read(fs, h, 0, buf, sizeof(buf)) == "hello");
        CHECK(read(fs, h, 3, buf, sizeof(buf)) == "lo");
        CHECK(read(fs, h, 5, buf, sizeof(buf)) == "");

        // A write past the end extends the file, zero-filling the gap.
        CHECK(write(fs, h, 7, "!").value() == 1);
        CHECK(fs.size(h).value() == 8);
        CHECK(read(fs, h, 0, buf, sizeof(buf)) == Str("hello\0\0!", 8));

        CHECK(fs.truncate(h, 5).is_ok());
        CHECK(fs.size(h).value() == 5);
        fs.close(h);
        CHECK(fs.size(h).is_err()); // the handle is gone with it

        // Reopening finds what was written; O_TRUNC does not.
        h = open(fs, "/notes", O_READ).value();
        CHECK(read(fs, h, 0, buf, sizeof(buf)) == "hello");
        fs.close(h);
        h = open(fs, "/notes", O_WRITE | O_TRUNC).value();
        CHECK(fs.size(h).value() == 0);
        fs.close(h);

        // Directories, and the errors that keep the two kinds apart.
        CHECK(run_now(fs.mkdir("/work")).is_ok());
        CHECK(run_now(fs.mkdir("/work")).error() == Error::Exists);
        CHECK(run_now(fs.mkdir("/nowhere/deep")).error() == Error::NotFound);
        CHECK(open(fs, "/work", O_READ).error() == Error::IsDir);
        CHECK(run_now(fs.list("/notes")).error() == Error::NotDir);

        CHECK(open(fs, "/work/a", O_WRITE | O_CREATE).is_ok());
        Vec<Entry> root = move(run_now(fs.list("/")).value());
        CHECK_EQ(root.size(), 2);
        CHECK(has(root, "notes", NodeKind::File));
        CHECK(has(root, "work", NodeKind::Dir));

        // An open file cannot be removed: nothing here has unlink semantics,
        // so the handle would be left pointing at a freed node.
        CHECK(run_now(fs.remove("/work", false)).error() == Error::NotEmpty);
        CHECK(run_now(fs.remove("/work/a", false)).error() == Error::Perm);

        Vec<Entry> work = move(run_now(fs.list("/work")).value());
        CHECK_EQ(work.size(), 1);
        for (u32 i = 0; i < 4; i++)
            fs.close(i); // whichever of them is still open

        CHECK(run_now(fs.remove("/work", true)).is_ok());
        CHECK(run_now(fs.stat("/work")).error() == Error::NotFound);
        CHECK(run_now(fs.remove("/", true)).error() == Error::Perm);

        CHECK(fs.bytes() == 0); // notes was truncated, and work is gone
    }
    CHECK_EQ(heap_stats().bytes_in_use, in_use);
}

#include "fs/bundlefs.h"
#include "harness.h"
#include "kernel/alloc.h"

namespace {

constexpr usize HEADER = 12;
constexpr usize SLOT   = 16;

void put_u32(String &s, u32 v)
{
    for (u32 i = 0; i < 4; i++)
        CHECK(s.push(char((v >> (i * 8)) & 0xff)));
}

// The other half of tools/pack.py, so a change to the format has to break one
// of the two loudly rather than both quietly.
String pack(Span<const Str> names, Span<const Str> data)
{
    String out;
    CHECK(out.append("BBND"));
    put_u32(out, 1);
    put_u32(out, u32(names.size()));

    u32 at = u32(HEADER + SLOT * names.size());
    for (usize i = 0; i < names.size(); i++) {
        put_u32(out, at);
        put_u32(out, u32(names[i].size()));
        put_u32(out, at + u32(names[i].size()));
        put_u32(out, u32(data[i].size()));
        at += u32(names[i].size() + data[i].size());
    }
    for (usize i = 0; i < names.size(); i++) {
        CHECK(out.append(names[i]));
        CHECK(out.append(data[i]));
    }
    return out;
}

bool has(const Vec<Entry> &v, Str name, NodeKind kind)
{
    for (const Entry &e : v)
        if (e.name == name && e.kind == kind)
            return true;
    return false;
}

} // namespace

void test_bundlefs()
{
    test_begin("bundlefs");

    usize in_use = heap_stats().bytes_in_use;
    {
        Str names[] = { "share/motd", "share/doc/README", "top" };
        Str data[]  = { "hello\n", "read me\n", "x" };

        Result<BundleFs *> r = bundlefs_create(pack(names, data));
        CHECK(r.is_ok());
        BundleFs &fs = *r.value();
        char buf[64];

        // Directories are implicit: they exist because something is under them.
        CHECK(run_now(fs.stat("/")).value().kind == NodeKind::Dir);
        CHECK(run_now(fs.stat("/share")).value().kind == NodeKind::Dir);
        CHECK(run_now(fs.stat("/share/doc")).value().kind == NodeKind::Dir);
        CHECK(run_now(fs.stat("/share/motd")).value().kind == NodeKind::File);
        CHECK(run_now(fs.stat("/share/motd")).value().size == 6);
        CHECK(run_now(fs.stat("/nope")).error() == Error::NotFound);

        Vec<Entry> root = move(run_now(fs.list("/")).value());
        CHECK_EQ(root.size(), 2);
        CHECK(has(root, "share", NodeKind::Dir));
        CHECK(has(root, "top", NodeKind::File));

        Vec<Entry> share = move(run_now(fs.list("/share")).value());
        CHECK_EQ(share.size(), 2);
        CHECK(has(share, "motd", NodeKind::File));
        CHECK(has(share, "doc", NodeKind::Dir));
        CHECK(run_now(fs.list("/nope")).error() == Error::NotFound);

        u32 h = run_now(fs.open("/share/doc/README", O_READ)).value();
        CHECK(fs.size(h).value() == 8);
        Result<usize> n = fs.read(h, 0, reinterpret_cast<u8 *>(buf), sizeof(buf));
        CHECK(n.is_ok());
        CHECK(Str(buf, n.value()) == "read me\n");
        n = fs.read(h, 5, reinterpret_cast<u8 *>(buf), sizeof(buf));
        CHECK(Str(buf, n.value()) == "me\n");
        fs.close(h);

        // Read-only means every writing path is refused, not just the ones the
        // VFS happens to check.
        CHECK(!fs.writable());
        CHECK(run_now(fs.open("/new", O_WRITE | O_CREATE)).error() == Error::Perm);
        CHECK(run_now(fs.mkdir("/new")).error() == Error::Perm);
        CHECK(run_now(fs.remove("/top", false)).error() == Error::Perm);
        CHECK(fs.truncate(0, 0).error() == Error::Perm);

        heap_delete(&fs);
    }

    // A header that does not check out is a packing bug, and says so.
    {
        String bad;
        CHECK(bad.append("BBNX"));
        put_u32(bad, 1);
        put_u32(bad, 0);
        CHECK(bundlefs_create(move(bad)).error() == Error::Invalid);

        String truncated;
        CHECK(truncated.append("BBND"));
        put_u32(truncated, 1);
        put_u32(truncated, 99); // more entries than there are bytes for
        CHECK(bundlefs_create(move(truncated)).error() == Error::Invalid);
    }

    CHECK_EQ(heap_stats().bytes_in_use, in_use);
}

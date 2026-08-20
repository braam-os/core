#include "cmd/pkg/encode.h"
#include "cmd/pkg/sha256.h"
#include "cmd/pkg/zip.h"
#include "fs/opfsfs.h"
#include "fs/vfs.h"
#include "harness.h"
#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/sched.h"
#include "svc/net.h"

namespace {

// ------------------------------------------------------------- the builder

// One entry, its central record and an end record, written by hand — the C++
// twin of run.mjs's zipOf, with the fields a refusal needs to lie about.
// tools/pack.py always deflates, writes no directory entry and no extra field,
// so nothing below exists anywhere else in the tree.
constexpr u32 KEEP = 0xfffffffe;

struct Made {
    u32 method        = ZIP_STORE;
    u32 flags         = 0;
    u32 declared      = KEEP; // the uncompressed size both records carry
    u32 packed        = KEEP; // the compressed size the central record carries
    u32 local_extra   = 0;    // padding between the local header and the data
    u32 central_extra = 0;
    u32 local_at      = KEEP; // what the central record points at
    u32 count         = KEEP; // what the end record declares
    u32 catalog       = KEEP; // where it says the directory is
    usize comment     = 0;
    bool local_sizes  = true; // false: zeroes, as flag bit 3 leaves them
    bool local_sig    = true;
    usize truncate    = 0; // bytes cut off the end, before the comment
};

struct Zipper {
    String out;

    void u8v(u32 v) { out.push(char(v & 0xff)); }
    void u16v(u32 v) { u8v(v), u8v(v >> 8); }
    void u32v(u32 v) { u16v(v & 0xffff), u16v(v >> 16); }
    void put(Str s) { out.append(s); }
};

String zip_make(Str name, Str data, const Made &m)
{
    Zipper z;
    u32 declared = m.declared == KEEP ? u32(data.size()) : m.declared;
    u32 packed   = m.packed == KEEP ? u32(data.size()) : m.packed;

    z.u32v(m.local_sig ? 0x04034b50 : 0x04034b51);
    z.u16v(20), z.u16v(m.flags), z.u16v(m.method), z.u16v(0), z.u16v(0);
    z.u32v(0);
    z.u32v(m.local_sizes ? packed : 0);
    z.u32v(m.local_sizes ? declared : 0);
    z.u16v(u32(name.size())), z.u16v(m.local_extra);
    z.put(name);
    for (u32 i = 0; i < m.local_extra; i++)
        z.u8v('x');
    z.put(data);

    u32 catalog = u32(z.out.size());
    z.u32v(0x02014b50);
    z.u16v(20), z.u16v(20), z.u16v(m.flags), z.u16v(m.method), z.u16v(0), z.u16v(0);
    z.u32v(0), z.u32v(packed), z.u32v(declared);
    z.u16v(u32(name.size())), z.u16v(m.central_extra), z.u16v(0);
    z.u16v(0), z.u16v(0), z.u32v(0);
    z.u32v(m.local_at == KEEP ? 0 : m.local_at);
    z.put(name);
    for (u32 i = 0; i < m.central_extra; i++)
        z.u8v('y');

    for (usize i = 0; i < m.truncate; i++)
        z.out.pop();

    u32 end = u32(z.out.size());
    z.u32v(0x06054b50);
    z.u16v(0), z.u16v(0);
    z.u16v(m.count == KEEP ? 1 : m.count), z.u16v(m.count == KEEP ? 1 : m.count);
    z.u32v(end - catalog);
    z.u32v(m.catalog == KEEP ? catalog : m.catalog);
    z.u16v(u32(m.comment));
    for (usize i = 0; i < m.comment; i++)
        z.u8v('c');
    return move(z.out);
}

// A case that reads nothing back, so the entries need not outlive the archive
// they view. Everything that does look at an entry keeps its own String.
ZipRead refused(Str name, const Made &m)
{
    Vec<ZipEntry> v;
    String zip = zip_make(name, "hi", m);
    return zip_entries(zip.str(), v);
}

// ------------------------------------------------------ the rootfs.zip case

// The digest as lowercase hex, which is how the manifest is written.
struct ZipDigest {
    char text[hex_size(SHA256_SIZE)];

    Str str() const { return Str(text, sizeof(text)); }
};

ZipDigest hex_of(const u8 d[SHA256_SIZE])
{
    ZipDigest out;
    hex_encode(Bytes(d, SHA256_SIZE), Span<char>(out.text));
    return out;
}

// The kernel's own inflate, which is as far as tests.wasm reaches: a syscall
// needs a program. src/cmd/pkg/unzip.cpp is the same loop over Sys::Inflate,
// and the ceiling both stop at is zip.h's ZipSink either way.
Task<Result<String>> inflate_entry(const ZipEntry &e)
{
    Result<HttpResponse> open = Err(Error::NoMemory);
    if (Task<Result<HttpResponse>> t = svc_inflate(e.data))
        open = co_await t;
    if (open.is_err())
        co_return Err(open.error());

    ZipSink sink(e.size);
    for (;;) {
        Result<String> chunk = Err(Error::NoMemory);
        if (Task<Result<String>> t = stream_read(open.value()))
            chunk = co_await t;
        if (chunk.is_err())
            co_return Err(chunk.error());
        if (chunk.value().empty())
            break; // the end of the stream
        if (!sink.take(chunk.value().str()))
            co_return Err(Error::Invalid); // past the declared size
    }
    if (!sink.complete())
        co_return Err(Error::Invalid);
    co_return move(sink.text());
}

Task<Result<Vec<u8>>> slurp(Str path)
{
    Result<i32> open = Err(Error::NoMemory);
    if (Task<Result<i32>> t = vfs_open(path, O_READ))
        open = co_await t;
    if (open.is_err())
        co_return Err(open.error());
    i32 fd = open.value();

    Vec<u8> out;
    Result<u64> size = vfs_size(fd);
    if (size.is_err() || !out.resize(usize(size.value()))) {
        vfs_close(fd);
        co_return Err(size.is_err() ? size.error() : Error::NoMemory);
    }
    usize at = 0;
    while (at < out.size()) {
        Result<usize> n = vfs_read(fd, at, out.data() + at, out.size() - at);
        if (n.is_err() || n.value() == 0) {
            vfs_close(fd);
            co_return Err(n.is_err() ? n.error() : Error::Io);
        }
        at += n.value();
    }
    vfs_close(fd);
    co_return move(out);
}

Str text_of(const Vec<u8> &v)
{
    return Str(reinterpret_cast<const char *>(v.data()), v.size());
}

u32 compared; // entries checked against the manifest
bool rootfs_ok;
Buf<128> rootfs_why; // whichever entry disagreed, or how the read failed

void blame(Str what, Str who)
{
    rootfs_why = {};
    rootfs_why.put(what);
    if (!who.empty())
        rootfs_why.put(": ").put(who);
}

// Every entry of the real archive, against the digests web/fs.js's parseZip
// produced from the same bytes in run.mjs.
Task<i32> ask_rootfs()
{
    compared  = 0;
    rootfs_ok = false;
    blame("nothing ran", "");

    Result<Vec<u8>> zip = Err(Error::NoMemory);
    if (Task<Result<Vec<u8>>> t = slurp("/rootfs.zip"))
        zip = co_await t;
    Result<Vec<u8>> manifest = Err(Error::NoMemory);
    if (Task<Result<Vec<u8>>> t = slurp("/rootfs.manifest"))
        manifest = co_await t;
    if (zip.is_err() || manifest.is_err()) {
        blame("the archive would not read",
              error_name(zip.is_err() ? zip.error() : manifest.error()));
        co_return 1;
    }

    Vec<ZipEntry> entries;
    if (zip_entries(text_of(zip.value()), entries) != ZipRead::Ok) {
        blame("the archive would not parse", "");
        co_return 1;
    }

    Str rest = text_of(manifest.value());
    for (const ZipEntry &e : entries) {
        Str line = rest.split('\n', rest);
        Str name = line.split(' ', line);
        Str size = line.split(' ', line);
        if (name != e.name) {
            blame("a name web/fs.js did not give", e.name);
            co_return 1;
        }

        Result<String> body = Err(Error::NoMemory);
        if (Task<Result<String>> t = inflate_entry(e))
            body = co_await t;
        if (body.is_err()) {
            blame(error_name(body.error()), e.name);
            co_return 1;
        }

        Buf<16> want;
        want.put(body.value().size());
        if (want.str() != size) {
            blame("a size web/fs.js did not give", e.name);
            co_return 1;
        }

        u8 d[SHA256_SIZE];
        sha256(Bytes(reinterpret_cast<const u8 *>(body.value().data()), body.value().size()), d);
        if (hex_of(d).str() != line) {
            blame("bytes web/fs.js did not give", e.name);
            co_return 1;
        }
        compared++;
    }

    rootfs_ok = rest == "\n" || rest.empty();
    if (!rootfs_ok)
        blame("the manifest had entries the archive did not", "");
    co_return 0;
}

} // namespace

void test_zip()
{
    test_begin("zip");

    usize in_use = heap_stats().bytes_in_use;

    // §5.2: a stored entry, read back whole.
    {
        Vec<ZipEntry> v;
        String zip = zip_make("share/hello", "hi", {});
        CHECK(zip_entries(zip.str(), v) == ZipRead::Ok);
        CHECK_EQ(v.size(), 1);
        CHECK(v[0].name == "share/hello");
        CHECK(v[0].data == "hi");
        CHECK_EQ(u32(v[0].size), 2);
        Str body;
        CHECK(zip_stored(v[0], body));
        CHECK(body == "hi");
    }

    // The local header is re-read, so an extra field the central record does
    // not have still lands on the data. Taking the central directory's offset
    // is the classic way to get this wrong, and rootfs.zip cannot catch it:
    // pack.py writes no extra field at either end.
    {
        Vec<ZipEntry> v;
        Made m;
        m.local_extra = 7;
        String zip    = zip_make("a", "hi", m);
        CHECK(zip_entries(zip.str(), v) == ZipRead::Ok);
        CHECK(v.size() == 1 && v[0].data == "hi");
    }
    {
        Vec<ZipEntry> v;
        Made m;
        m.central_extra = 11;
        String zip      = zip_make("a", "hi", m);
        CHECK(zip_entries(zip.str(), v) == ZipRead::Ok);
        CHECK(v.size() == 1 && v[0].data == "hi");
    }

    // Flag bit 3 leaves the local sizes zero; the compressed size comes from
    // the central record and nowhere else.
    {
        Vec<ZipEntry> v;
        Made m;
        m.flags       = 8;
        m.local_sizes = false;
        String zip    = zip_make("a", "hi", m);
        CHECK(zip_entries(zip.str(), v) == ZipRead::Ok);
        CHECK(v.size() == 1 && v[0].data == "hi");
    }

    // The end record, behind a comment and past the window.
    {
        Vec<ZipEntry> v;
        Made m;
        m.comment  = 100;
        String zip = zip_make("a", "hi", m);
        CHECK(zip_entries(zip.str(), v) == ZipRead::Ok);
    }
    {
        Vec<ZipEntry> v;
        Made m;
        m.comment  = 0x10000;
        String zip = zip_make("a", "hi", m);
        CHECK(zip_entries(zip.str(), v) == ZipRead::Malformed);
    }

    // No end record at all, and an archive too short to hold one.
    {
        Vec<ZipEntry> v;
        CHECK(zip_entries("", v) == ZipRead::Malformed);
        CHECK(zip_entries("PK", v) == ZipRead::Malformed);
        String zip = zip_make("a", "hi", {});
        CHECK(zip_entries(zip.str().substr(0, zip.size() - 4), v) == ZipRead::Malformed);
    }

    // Zip64 announces itself by saturating either field.
    {
        Made m;
        m.count = 0xffff;
        CHECK(refused("a", m) == ZipRead::Unsupported);
        Made n;
        n.catalog = 0xffffffff;
        CHECK(refused("a", n) == ZipRead::Unsupported);
    }

    // Encrypted, and a method that is neither 0 nor 8.
    {
        Made m;
        m.flags = 1;
        CHECK(refused("a", m) == ZipRead::Unsupported);
        Made n;
        n.method = 12;
        CHECK(refused("a", n) == ZipRead::Unsupported);
    }

    // Truncation, three ways: the directory cut short, a local header that is
    // not one, and data running past the end.
    {
        Made m;
        m.truncate = 8;
        CHECK(refused("a", m) == ZipRead::Malformed);
        Made n;
        n.local_sig = false;
        CHECK(refused("a", n) == ZipRead::Malformed);
        Made o;
        o.packed = 4096;
        CHECK(refused("a", o) == ZipRead::Malformed);
        Made p;
        p.local_at = 0xfffffff0;
        CHECK(refused("a", p) == ZipRead::Malformed);
        Made q;
        q.count = 2;
        CHECK(refused("a", q) == ZipRead::Malformed);
    }

    // A directory entry is skipped and not refused — the `/` test runs before
    // the name test, which is what web/fs.js does.
    {
        Vec<ZipEntry> v;
        String zip = zip_make("share/", "", {});
        CHECK(zip_entries(zip.str(), v) == ZipRead::Ok);
        CHECK_EQ(v.size(), 0);
        Vec<ZipEntry> w;
        String bad = zip_make("../", "", {});
        CHECK(zip_entries(bad.str(), w) == ZipRead::Ok);
        CHECK_EQ(w.size(), 0);
    }

    // A name out of an archive is not a path until it has been looked at.
    constexpr Str HOSTILE[] = { "",        "/etc/passwd", "../escape", "bin/../../out",
                                "C:\\out", "a\\b",        "./x",       "x/./y",
                                "x/../y",  "..",          "." };
    for (Str name : HOSTILE) {
        Vec<ZipEntry> v;
        String zip = zip_make(name, "hi", {});
        // The name is the expression, so a failure names the case.
        test_check(zip_entries(zip.str(), v) == ZipRead::Malformed, name, __FILE_NAME__, __LINE__);
    }

    constexpr Str SAFE[] = { "bin/.keep", "share/x", "a..b", "...", "a..", "1:x" };
    for (Str name : SAFE) {
        Vec<ZipEntry> v;
        String zip = zip_make(name, "hi", {});
        test_check(zip_entries(zip.str(), v) == ZipRead::Ok, name, __FILE_NAME__, __LINE__);
    }

    // §5.1: a top-level dot-entry is metadata, and an unknown one makes the
    // package uninstallable.
    struct MetaCase {
        Str name;
        ZipMeta meta;
    };
    constexpr MetaCase META[] = {
        { ".PKGINFO", ZipMeta::PkgInfo },
        { ".pre-install", ZipMeta::PreInstall },
        { ".post-install", ZipMeta::PostInstall },
        { ".pre-deinstall", ZipMeta::PreDeinstall },
        { ".post-deinstall", ZipMeta::PostDeinstall },
        { ".pre-upgrade", ZipMeta::PreUpgrade },
        { ".post-upgrade", ZipMeta::PostUpgrade },
        { ".trigger", ZipMeta::Trigger },

        { ".evil", ZipMeta::Unknown },
        { ".PKGINFO2", ZipMeta::Unknown },

        // Only a name with no `/` can be metadata.
        { "bin/.keep", ZipMeta::Payload },
        { "d/.PKGINFO", ZipMeta::Payload },
        { "share/x", ZipMeta::Payload },
        { "", ZipMeta::Payload },
    };
    for (const MetaCase &c : META)
        test_check(zip_meta(c.name) == c.meta, c.name, __FILE_NAME__, __LINE__);

    // The entry that refuses the package it is in.
    {
        Vec<ZipEntry> v;
        String zip = zip_make(".evil", "hi", {});
        CHECK(zip_entries(zip.str(), v) == ZipRead::Ok);
        CHECK(v.size() == 1 && zip_meta(v[0].name) == ZipMeta::Unknown);
    }

    // The ceiling. Sys::Inflate caps its input and not its output, so this is
    // what stops a bomb, and nothing below it knows what the entry claimed.
    {
        ZipSink exact(5);
        CHECK(exact.take("bra"));
        CHECK(!exact.complete());
        CHECK(exact.take("am"));
        CHECK(exact.complete());
        CHECK(exact.text().str() == "braam");

        ZipSink over(4);
        CHECK(over.take("bra"));
        CHECK(!over.take("am")); // one byte past, and nothing is taken
        CHECK_EQ(over.text().size(), 3);

        ZipSink short_(9);
        CHECK(short_.take("braam"));
        CHECK(!short_.complete()); // a stream that ends early is not a short read

        ZipSink nothing(0);
        CHECK(nothing.complete());
        CHECK(!nothing.take("x"));
    }

    CHECK_EQ(heap_stats().bytes_in_use, in_use);

    // rootfs.zip, entry for entry against what web/fs.js made of the same
    // bytes. run.mjs plants the archive and the digests it took; the inflate
    // is the kernel's own, since tests.wasm cannot run a program.
    usize live = jsref_live();
    sched_reset();
    CHECK(vfs_mount("/", heap_new<OpfsFs>()).is_ok());
    CHECK(sched_spawn(ask_rootfs()) != 0);
    CHECK_EQ(sched_tick(0), -1);
    test_check(rootfs_ok, rootfs_why.str(), __FILE_NAME__, __LINE__);
    CHECK_EQ(compared, 44);
    CHECK_EQ(jsref_live(), live);
    CHECK_EQ(host_orphans(), 0);
    vfs_reset();
}

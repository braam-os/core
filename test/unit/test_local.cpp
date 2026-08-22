#include "cmd/pkg/local.h"
#include "harness.h"

// What a sideload may claim (Package_Management.md §7). No archive is built:
// local_stanza takes the entry list and the .PKGINFO text, and a ZipEntry is a
// name and a size — zip.cpp's reading of a real one is test_zip.cpp's.

namespace {

constexpr Str DIGEST = "Q2IgfM18bBUW8blv5C1wE491Z5bfWNc+VRhcgcX1hLHUI=";

bool entry(Vec<ZipEntry> &v, Str name)
{
    ZipEntry e;
    e.name = name;
    return v.push(e);
}

// The archive's bytes are what C and S are taken from, so any text will do as
// long as the two agree with it.
bool load(LocalPackage &p, Str info, Str bytes)
{
    return p.info.assign(info) && p.zip.assign(bytes);
}

} // namespace

void test_local()
{
    test_begin("local");

    // An operand holding :// is fetched, one ending .zip is read off the
    // filesystem, and anything else is the §6 token it always was. `.zip`
    // alone is a name: the suffix has to be a suffix of something.
    CHECK(operand_kind("less") == Operand::Name);
    CHECK(operand_kind("cmd:awk") == Operand::Name);
    CHECK(operand_kind("less>=1.6") == Operand::Name);
    CHECK(operand_kind(".zip") == Operand::Name);
    CHECK(operand_kind("hello-1.0-r0.zip") == Operand::File);
    CHECK(operand_kind("/tmp/h.zip") == Operand::File);
    CHECK(operand_kind("./h.zip") == Operand::File);
    CHECK(operand_kind("https://h/x.zip") == Operand::Url);
    CHECK(operand_kind("http://h/x") == Operand::Url);
    // The URL test runs first, so a fetched archive is a URL and not a file.
    CHECK(operand_kind("https://h/a.zip") == Operand::Url);

    // .PKGINFO is the stanza: C is the digest of the archive and S its size,
    // and §6.1's cmd: names come off the flat bin/ entries — what mkindex.py
    // would have written into an index stanza, so a solve against the
    // installed set sees what a solve against an index would have.
    {
        Vec<ZipEntry> e;
        CHECK(entry(e, ".PKGINFO") && entry(e, "bin/hi") && entry(e, "bin/there"));
        CHECK(entry(e, "bin/sub/deep") && entry(e, "share/man/hi.1") && entry(e, ".post-install"));

        LocalPackage p;
        CHECK(load(p, "P:hello\nV:1.0-r0\nT:a greeting\nD:cmd:sh\n", "0123456789"));

        LocalStep step = LocalStep::Read;
        CHECK(local_stanza(p, e, step).is_ok());
        CHECK(p.stanza.name == "hello" && p.stanza.version == "1.0-r0");
        CHECK_EQ(u32(p.stanza.size), 10);
        CHECK(p.stanza.depends == "cmd:sh");
        // Flat entries only: bin/sub/deep is nothing the link farm carries.
        CHECK(p.stanza.provides == "cmd:hi=1.0-r0 cmd:there=1.0-r0");

        // The digest is the archive's, taken here rather than believed.
        u8 want[SHA256_SIZE];
        sha256(Bytes(reinterpret_cast<const u8 *>("0123456789"), 10), want);
        for (usize i = 0; i < SHA256_SIZE; i++)
            CHECK(p.stanza.digest[i] == want[i]);
    }

    // A p: written by hand merges with the derived names rather than being
    // replaced by them (§6.1).
    {
        Vec<ZipEntry> e;
        CHECK(entry(e, ".PKGINFO") && entry(e, "bin/hi"));
        LocalPackage p;
        CHECK(load(p, "P:hello\nV:2\np:editor\n", "x"));
        LocalStep step = LocalStep::Read;
        CHECK(local_stanza(p, e, step).is_ok());
        CHECK(p.stanza.provides == "editor cmd:hi=2");
    }

    // No bin/ is no cmd: names, and no p: at all.
    {
        Vec<ZipEntry> e;
        CHECK(entry(e, ".PKGINFO") && entry(e, "share/doc/readme"));
        LocalPackage p;
        CHECK(load(p, "P:doc\nV:1\n", "x"));
        LocalStep step = LocalStep::Read;
        CHECK(local_stanza(p, e, step).is_ok());
        CHECK(p.stanza.provides.empty());
    }

    // §5.1: an unknown top-level dot-entry makes the package uninstallable,
    // whoever vouched for it. A dot inside a directory is payload.
    {
        Vec<ZipEntry> e;
        CHECK(entry(e, ".PKGINFO") && entry(e, ".surprise"));
        LocalPackage p;
        CHECK(load(p, "P:a\nV:1\n", "x"));
        LocalStep step = LocalStep::Read;
        CHECK(local_stanza(p, e, step).is_err());
        CHECK(step == LocalStep::Metadata);

        Vec<ZipEntry> ok;
        CHECK(entry(ok, ".PKGINFO") && entry(ok, "bin/.keep"));
        LocalPackage q;
        CHECK(load(q, "P:a\nV:1\n", "x"));
        CHECK(local_stanza(q, ok, step).is_ok());
    }

    // The store path is built out of P and V, and a .PKGINFO's are nobody's to
    // vouch for: neither may climb out of /pkg/store.
    {
        Vec<ZipEntry> e;
        CHECK(entry(e, ".PKGINFO"));
        LocalPackage p;
        CHECK(load(p, "P:../../bin\nV:1\n", "x"));
        LocalStep step = LocalStep::Read;
        CHECK(local_stanza(p, e, step).is_err());
        CHECK(step == LocalStep::Pkginfo);

        LocalPackage q;
        CHECK(load(q, "P:a\nV:../1\n", "x"));
        CHECK(local_stanza(q, e, step).is_err());
        CHECK(step == LocalStep::Pkginfo);

        // A C inside the archive would be a claim about the archive.
        LocalPackage r;
        String info;
        CHECK(info.append("P:a\nV:1\nC:") && info.append(DIGEST) && info.push('\n'));
        CHECK(load(r, info.str(), "x"));
        CHECK(local_stanza(r, e, step).is_err());
        CHECK(step == LocalStep::Pkginfo);
    }

    // §7: the index stays authoritative wherever it speaks. A name-version it
    // lists at another digest is a conflict; the same digest is the same
    // package, carried by hand, and is none.
    {
        CheckedIndex c;
        PackageStanza listed;
        listed.name    = "less";
        listed.version = "1.6-r1";
        CHECK(digest_parse(DIGEST, listed.digest));
        CHECK(c.packages.push(listed));

        PackageStanza same = listed;
        CHECK(!local_conflicts(c, same));

        PackageStanza other = listed;
        other.digest[0] ^= 1;
        CHECK(local_conflicts(c, other));

        // A version the index does not list is not a conflict at all.
        PackageStanza fresh = other;
        fresh.version       = "9.9-r9";
        CHECK(!local_conflicts(c, fresh));

        PackageStanza elsewhere = other;
        elsewhere.name          = "mine";
        CHECK(!local_conflicts(c, elsewhere));
    }
}

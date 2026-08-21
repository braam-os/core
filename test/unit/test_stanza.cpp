#include "cmd/pkg/stanza.h"
#include "harness.h"

namespace {

// Package_Format.md §3's example, and §4's.
constexpr Str INDEX =
    "Y:ed25519 Q2key sig\n"
    "\n"
    "X:1\n"
    "N:https://packages.example/braam\n"
    "G:41\n"
    "E:1755648000000\n"
    "T:Braam packages\n"
    "\n"
    "C:Q2IgfM18bBUW8blv5C1wE491Z5bfWNc+VRhcgcX1hLHUI=\n"
    "P:awk\n"
    "V:1.2-r0\n"
    "S:18244\n"
    "I:41984\n"
    "T:pattern-directed scanning and processing language\n"
    "D:cmd:sh\n"
    "p:cmd:awk\n"
    "\n"
    "C:Q2IgfM18bBUW8blv5C1wE491Z5bfWNc+VRhcgcX1hLHUI=\n"
    "P:less\n"
    "V:1.6-r1\n"
    "S:9000\n";

constexpr Str ANCHOR =
    "Y:ed25519 Q2a s1\n"
    "Y:ed25519 Q2b s2\n"
    "\n"
    "X:1\n"
    "G:3\n"
    "E:1787184000000\n"
    "H:root 2\n"
    "H:index 1\n"
    "K:root ed25519 aaa\n"
    "K:root ed25519 bbb\n"
    "K:root ed25519 ccc\n"
    "K:index ed25519 ddd\n";

constexpr Str DIGEST = "Q2IgfM18bBUW8blv5C1wE491Z5bfWNc+VRhcgcX1hLHUI=";

// One stanza's worth of fields, or the read that refused it.
StanzaRead one(Str text, Str known, Vec<StanzaField> &f)
{
    StanzaReader r(text, known);
    return r.next(f);
}

} // namespace

void test_stanza()
{
    test_begin("stanza");

    // §1.1: decimal, unsigned, unpadded.
    CHECK(stanza_number("0").value() == 0);
    CHECK(stanza_number("41").value() == 41);
    CHECK(stanza_number("1755648000000").value() == 1755648000000ull);
    CHECK(stanza_number("18446744073709551615").value() == ~u64(0));
    CHECK(!stanza_number("007"));
    CHECK(!stanza_number("+1"));
    CHECK(!stanza_number("1a"));
    CHECK(!stanza_number(""));
    CHECK(!stanza_number(" 1"));
    CHECK(!stanza_number("18446744073709551616")); // one past

    // §1.1: Q2 and nothing else, over P6's strict decoder.
    {
        u8 d[SHA256_SIZE];
        CHECK(digest_parse(DIGEST, d));
        char text[DIGEST_TEXT];
        CHECK(digest_write(d, Span<char>(text)));
        CHECK(Str(text, sizeof(text)) == DIGEST);

        CHECK(!digest_parse("Q1IgfM18bBUW8blv5C1wE491Z5bfWNc+VRhcgcX1hLHUI=", d));
        CHECK(!digest_parse("X2IgfM18bBUW8blv5C1wE491Z5bfWNc+VRhcgcX1hLHUI=", d));
        CHECK(!digest_parse("Q2IgfM18bB", d));                                     // too short
        CHECK(!digest_parse("Q2IgfM18bBUW8blv5C1wE491Z5bfWNc+VRhcgcX1hLHUJ=", d)); // tail bits
        CHECK(!digest_parse("", d));

        char narrow[DIGEST_TEXT - 1];
        CHECK(!digest_write(d, Span<char>(narrow)));
    }

    // §2: the signed bytes are everything after the first empty line.
    {
        Str block, body;
        CHECK(signed_split(INDEX, block, body));
        CHECK(block == "Y:ed25519 Q2key sig\n");
        CHECK(body.starts_with("X:1\n"));

        CHECK(signed_split("\nX:1\n", block, body));
        CHECK(block.empty() && body == "X:1\n");

        CHECK(!signed_split("X:1\nY:2\n", block, body));
    }

    // §3: the index, end to end.
    {
        Str block, body;
        CHECK(signed_split(INDEX, block, body));

        Vec<StanzaField> f;
        CHECK(one(block, STANZA_SIGNATURE, f) == StanzaRead::Ok);
        Vec<Signature> sigs;
        CHECK(signature_read(f, sigs) == StanzaRead::Ok);
        CHECK_EQ(sigs.size(), 1);
        CHECK(sigs[0].algorithm == "ed25519" && sigs[0].key_name == "Q2key" &&
              sigs[0].signature == "sig");

        StanzaReader r(body, STANZA_HEADER);
        CHECK(r.next(f) == StanzaRead::Ok);
        IndexHeader h;
        CHECK(header_read(f, h) == StanzaRead::Ok);
        CHECK_EQ(h.grammar, 1);
        CHECK(h.url == "https://packages.example/braam");
        CHECK_EQ(u32(h.version), 41);
        CHECK(h.expiry == 1755648000000ull);
        CHECK(h.description == "Braam packages");

        // The header's letters are not a package's, so the rest is read again
        // with the package set — which is what a caller does.
        usize at = body.find("\n\n");
        StanzaReader p(body.substr(at + 2), STANZA_PACKAGE);
        PackageStanza pkg;
        CHECK(p.next(f) == StanzaRead::Ok);
        CHECK(package_read(f, pkg) == StanzaRead::Ok);
        CHECK(pkg.name == "awk" && pkg.version == "1.2-r0");
        CHECK_EQ(u32(pkg.size), 18244);
        CHECK_EQ(u32(pkg.installed_size), 41984);
        CHECK(pkg.depends == "cmd:sh" && pkg.provides == "cmd:awk");
        CHECK(pkg.install_if.empty() && pkg.origin.empty());

        CHECK(p.next(f) == StanzaRead::Ok);
        CHECK(package_read(f, pkg) == StanzaRead::Ok);
        CHECK(pkg.name == "less");
        // No trailing blank line, and the last stanza still commits.
        CHECK(p.next(f) == StanzaRead::End);
    }

    // §4: the anchor, thresholds and keys in order.
    {
        Str block, body;
        CHECK(signed_split(ANCHOR, block, body));
        Vec<StanzaField> f;
        CHECK(one(block, STANZA_SIGNATURE, f) == StanzaRead::Ok);
        Vec<Signature> sigs;
        CHECK(signature_read(f, sigs) == StanzaRead::Ok);
        CHECK_EQ(sigs.size(), 2);

        CHECK(one(body, STANZA_ANCHOR, f) == StanzaRead::Ok);
        Anchor a;
        CHECK(anchor_read(f, a) == StanzaRead::Ok);
        CHECK_EQ(u32(a.version), 3);
        CHECK_EQ(a.thresholds.size(), 2);
        CHECK(a.thresholds[0].use == "root" && a.thresholds[0].count == 2);
        CHECK(a.thresholds[1].use == "index" && a.thresholds[1].count == 1);
        CHECK_EQ(a.keys.size(), 4);
        CHECK(a.keys[0].use == "root" && a.keys[0].algorithm == "ed25519" &&
              a.keys[0].key == "aaa");
        CHECK(a.keys[3].use == "index" && a.keys[3].key == "ddd");
    }

    // §1: an unknown uppercase letter makes the record unusable, and the next
    // stanza still reads.
    {
        Str text = "C:Q2x\nP:a\nQ:junk\n\nC:";
        String two;
        CHECK(two.append(text) && two.append(DIGEST) && two.append("\nP:b\nV:1\nS:1\n"));
        Vec<StanzaField> f;
        StanzaReader r(two.str(), STANZA_PACKAGE);
        CHECK(r.next(f) == StanzaRead::Unusable);
        CHECK(r.next(f) == StanzaRead::Ok);
        PackageStanza p;
        CHECK(package_read(f, p) == StanzaRead::Ok);
        CHECK(p.name == "b");
    }

    // An unknown lowercase letter is ignored, and does not reach the fields.
    {
        Vec<StanzaField> f;
        CHECK(one("P:a\nq:junk\nV:1\n", STANZA_PACKAGE, f) == StanzaRead::Ok);
        CHECK_EQ(f.size(), 2);
        CHECK(f[0].letter == 'P' && f[1].letter == 'V');
    }

    // A repeat outside the accumulating six takes the file down; one of the
    // six does not. And a line that is not a field at all.
    {
        Vec<StanzaField> f;
        CHECK(one("P:a\nV:1\nV:2\n", STANZA_PACKAGE, f) == StanzaRead::Malformed);
        CHECK(one("F:d\nF:e\nP:a\n", STANZA_DB, f) != StanzaRead::Malformed);
        CHECK(one("P a\n", STANZA_PACKAGE, f) == StanzaRead::Malformed);
        CHECK(one("1:a\n", STANZA_PACKAGE, f) == StanzaRead::Malformed);
        CHECK(one("", STANZA_PACKAGE, f) == StanzaRead::End);
    }

    // §8.1: a record written and read back is byte-identical.
    {
        u8 d[SHA256_SIZE];
        CHECK(digest_parse(DIGEST, d));

        DbRecord r;
        r.pkg.name           = "awk";
        r.pkg.version        = "1.2-r0";
        r.pkg.size           = 18244;
        r.pkg.installed_size = 41984;
        r.pkg.description    = "one true awk";
        r.pkg.origin         = "awk";
        r.pkg.build_time     = 1755648000000ull;
        r.pkg.priority       = 5;
        r.pkg.globs          = "/bin /share";
        r.pkg.depends        = "cmd:sh so";
        r.pkg.provides       = "cmd:awk";
        r.pkg.install_if     = "less";
        r.index_version      = 41;
        for (usize i = 0; i < SHA256_SIZE; i++)
            r.pkg.digest[i] = d[i];

        DbFile a{ "bin", "awk", {} }, b{ "bin", "nawk", {} }, c{ "share/man", "awk.1", {} };
        for (usize i = 0; i < SHA256_SIZE; i++)
            a.digest[i] = b.digest[i] = c.digest[i] = d[i];
        CHECK(r.files.push(a) && r.files.push(b) && r.files.push(c));

        String text;
        CHECK(db_write(r, text));

        Vec<StanzaField> f;
        StanzaReader rd(text.str(), STANZA_DB);
        CHECK(rd.next(f) == StanzaRead::Ok);
        DbRecord back;
        CHECK(db_read(f, back) == StanzaRead::Ok);

        String again;
        CHECK(db_write(back, again));
        CHECK(again.str() == text.str());

        CHECK_EQ(back.files.size(), 3);
        CHECK(back.files[0].dir == "bin" && back.files[0].name == "awk");
        CHECK(back.files[2].dir == "share/man" && back.files[2].name == "awk.1");
        CHECK_EQ(u32(back.index_version), 41);
        CHECK(back.pkg.globs == "/bin /share" && back.pkg.priority == 5);
        CHECK(back.broken.empty());

        // b is §11's failed script. Lowercase, so an index stanza carrying one
        // drops it rather than being refused over it.
        r.broken = "post-install";
        String marked;
        CHECK(db_write(r, marked));
        CHECK(marked.str().find("G:41\nb:post-install\nF:bin\n") != Str::npos);

        Vec<StanzaField> g;
        DbRecord broke;
        CHECK(StanzaReader::one(marked.str(), STANZA_DB, g));
        CHECK(db_read(g, broke) == StanzaRead::Ok);
        CHECK(broke.broken == "post-install");

        // The same letter in an index stanza: lowercase, so it is dropped and
        // the record still reads. G, F, R and Z would have refused it.
        String plain;
        CHECK(package_write(r.pkg, plain));
        CHECK(plain.append("b:post-install\n"));

        Vec<StanzaField> h;
        PackageStanza ignored;
        CHECK(StanzaReader::one(plain.str(), STANZA_PACKAGE, h));
        CHECK(package_read(h, ignored) == StanzaRead::Ok);
        for (const StanzaField &x : h)
            CHECK(x.letter != 'b');
    }
}

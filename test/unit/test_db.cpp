#include "cmd/pkg/db.h"
#include "cmd/pkg/stanza.h"
#include "harness.h"

namespace {

// One expected step, as a row rather than four checks.
struct Step {
    StoreOpKind kind;
    Str path;
    Str data;
};

void expect(Span<const StoreOp> got, Span<const Step> want, Str who, u32 line)
{
    test_check_eq(u32(got.size()), u32(want.size()), who, __FILE_NAME__, line);
    for (usize i = 0; i < got.size() && i < want.size(); i++)
        test_check(got[i].kind == want[i].kind && got[i].path == want[i].path &&
                       got[i].data == want[i].data,
                   got[i].path.str(), __FILE_NAME__, line);
}

constexpr Installed TWO[] = {
    { "less", "1.6-r1" },
    { "awk", "1.2-r0" },
};

constexpr Str TWO_TEXT = "awk 1.2-r0\nless 1.6-r1\n";

constexpr GenLink LINKS[] = {
    { "awk", "awk", "1.2-r0" },
    { "less", "less", "1.6-r1" },
};

constexpr Step TREE[] = {
    { StoreOpKind::MkDir, "/pkg", "" },       { StoreOpKind::MkDir, "/pkg/store", "" },
    { StoreOpKind::MkDir, "/pkg/db", "" },    { StoreOpKind::MkDir, "/pkg/gen", "" },
    { StoreOpKind::MkDir, "/pkg/cache", "" }, { StoreOpKind::Link, "/pkg/bin", "/pkg/active/bin" },
};

// The whole of a commit, in the order it happens.
constexpr Step GEN[] = {
    { StoreOpKind::Remove, "/pkg/gen/2", "" },
    { StoreOpKind::MkDir, "/pkg/gen/2", "" },
    { StoreOpKind::Write, "/pkg/gen/2/packages", TWO_TEXT },
    { StoreOpKind::MkDir, "/pkg/gen/2/bin", "" },
    { StoreOpKind::Link, "/pkg/gen/2/bin/awk", "/pkg/store/awk-1.2-r0/bin/awk" },
    { StoreOpKind::Link, "/pkg/gen/2/bin/less", "/pkg/store/less-1.6-r1/bin/less" },
    { StoreOpKind::Link, "/pkg/active.new", "/pkg/gen/2" },
    { StoreOpKind::Rename, "/pkg/active.new", "/pkg/active" },
};

struct GenCase {
    Str target;
    u32 want;
};

constexpr GenCase GENERATIONS[] = {
    { "/pkg/gen/2", 2 },
    { "gen/2", 2 }, // however the link was spelled
    { "/pkg/gen/12", 12 },
    { "/pkg/gen/999999", 999999 },

    { "gen/0", 0 }, // 0 is not a generation
    { "/pkg/gen/x", 0 },
    { "/pkg/gen/", 0 },
    { "/pkg/gen/2x", 0 },
    { "/pkg/gen/ 2", 0 },
    { "/pkg/store/2", 0 },
    { "/pkg/2", 0 },
    { "2", 0 },
    { "", 0 },
    { "/pkg/gen/1234567890", 0 }, // wider than a generation ever gets
};

constexpr Str MALFORMED[] = {
    "awk\n",              // one field
    "awk 1.2-r0 extra\n", // three
    "awk \n",
    " 1.2-r0\n",
    "awk  1.2-r0\n",
};

} // namespace

void test_db()
{
    test_begin("db");

    // §8's paths, built rather than pasted.
    {
        String s;
        CHECK(pkg_stem("awk", "1.2-r0", s) && s.str() == "awk-1.2-r0");
        CHECK(pkg_store_dir("awk", "1.2-r0", "", s) && s.str() == "/pkg/store/awk-1.2-r0");
        CHECK(pkg_store_dir("awk", "1.2-r0", "bin/awk", s) &&
              s.str() == "/pkg/store/awk-1.2-r0/bin/awk");
        CHECK(pkg_db_file("awk", "1.2-r0", s) && s.str() == "/pkg/db/awk-1.2-r0");
        CHECK(pkg_gen_dir(2, "", s) && s.str() == "/pkg/gen/2");
        CHECK(pkg_gen_dir(2, "packages", s) && s.str() == "/pkg/gen/2/packages");
    }

    for (const GenCase &c : GENERATIONS)
        test_check(gen_of(c.target) == c.want, c.target, __FILE_NAME__, __LINE__);

    // §8.2: a generation written and read back is the same, and writing it
    // again is byte-identical. The input is unsorted; the writer sorts.
    {
        String text;
        CHECK(packages_write(Span<const Installed>(TWO), text));
        CHECK(text.str() == TWO_TEXT);

        Vec<Installed> back;
        CHECK(packages_read(text.str(), back));
        CHECK_EQ(back.size(), 2);
        CHECK(back[0].name == "awk" && back[0].version == "1.2-r0");
        CHECK(back[1].name == "less" && back[1].version == "1.6-r1");

        String again;
        CHECK(packages_write(Span<const Installed>(back), again));
        CHECK(again.str() == text.str());
    }

    // A blank line is skipped and a missing final newline is still a line.
    {
        Vec<Installed> v;
        CHECK(packages_read("\n\nawk 1.2-r0\n\nless 1.6-r1", v));
        CHECK_EQ(v.size(), 2);
        CHECK(v[1].version == "1.6-r1");

        Vec<Installed> none;
        CHECK(packages_read("", none));
        CHECK_EQ(none.size(), 0);
    }

    for (Str text : MALFORMED) {
        Vec<Installed> v;
        test_check(!packages_read(text, v), text, __FILE_NAME__, __LINE__);
    }

    // world is §6 tokens, one per line, kept as written.
    {
        constexpr Str DEPS[] = { "awk", "!foo", "less>=1.2", "cmd:awk" };
        String text;
        CHECK(world_write(Span<const Str>(DEPS), text));
        CHECK(text.str() == "awk\n!foo\nless>=1.2\ncmd:awk\n");

        Vec<Str> back;
        CHECK(world_read(text.str(), back));
        CHECK_EQ(back.size(), 4);
        CHECK(back[1] == "!foo" && back[3] == "cmd:awk");
    }

    {
        Vec<Str> urls;
        CHECK(repos_read("https://a/x\n\nhttps://b/y\n", urls));
        CHECK_EQ(urls.size(), 2);
        CHECK(urls[0] == "https://a/x" && urls[1] == "https://b/y");
    }

    // The tree from nothing, and the commit, step for step.
    {
        Vec<StoreOp> ops;
        CHECK(pkg_tree_ops(ops));
        expect(Span<const StoreOp>(ops), Span<const Step>(TREE), "pkg_tree_ops", __LINE__);
    }
    {
        Vec<StoreOp> ops;
        CHECK(gen_ops(2, Span<const Installed>(TWO), Span<const GenLink>(LINKS), ops));
        expect(Span<const StoreOp>(ops), Span<const Step>(GEN), "gen_ops", __LINE__);
    }

    // A generation with nothing in it is still a generation, and still commits.
    {
        Vec<StoreOp> ops;
        CHECK(gen_ops(1, {}, {}, ops));
        CHECK_EQ(ops.size(), 6);
        CHECK(ops[2].path.str() == "/pkg/gen/1/packages" && ops[2].data.empty());
        CHECK(ops[5].kind == StoreOpKind::Rename);
        CHECK(ops[5].path.str() == "/pkg/active.new" && ops[5].data.str() == "/pkg/active");
    }

    // An operand joins world by name: a token already there is replaced where
    // it stands, and one that says the same thing changes nothing.
    {
        Vec<Str> specs;
        bool changed = false;
        CHECK(world_read("awk\nless>=1.6\n", specs));
        CHECK(world_push(specs, "less", changed) && changed);
        CHECK_EQ(specs.size(), 2);
        CHECK(specs[0] == "awk" && specs[1] == "less");

        CHECK(world_push(specs, "less", changed) && !changed);
        CHECK(world_push(specs, "hello", changed) && changed);
        CHECK_EQ(specs.size(), 3);
        CHECK(specs[2] == "hello");

        String out;
        CHECK(world_write(specs, out));
        CHECK(out.str() == "awk\nless\nhello\n");
    }

    // Out again, by name: the line goes whatever constraint it carried, and
    // what was never there is not a change.
    {
        Vec<Str> specs;
        CHECK(world_read("awk\nless>=1.6\nhello\n", specs));
        CHECK(world_drop(specs, "less"));
        CHECK_EQ(specs.size(), 2);
        CHECK(specs[0] == "awk" && specs[1] == "hello");
        CHECK(!world_drop(specs, "less"));
        CHECK(!world_drop(specs, "nonesuch"));
        CHECK_EQ(specs.size(), 2);

        String out;
        CHECK(world_write(specs, out));
        CHECK(out.str() == "awk\nhello\n");

        // Down to nothing, which is a world with no lines and not a failure.
        CHECK(world_drop(specs, "awk") && world_drop(specs, "hello"));
        CHECK(specs.empty());
        String none;
        CHECK(world_write(specs, none));
        CHECK(none.empty());
    }

    // The lines as dependencies, and one that is not a token at all is not.
    {
        Vec<Str> specs;
        Vec<Dep> deps;
        CHECK(world_read("awk\n<broken\nless>=1.6\n", specs));
        CHECK(world_deps(specs, deps));
        CHECK_EQ(deps.size(), 2);
        CHECK(deps[0].name == "awk" && deps[1].name == "less" && deps[1].version == "1.6");
    }

    // §8.1's F and R. A file at the top of a package has an empty F, which is
    // one character from being unwritable and is what this pins.
    {
        Str dir, name;
        db_split("bin/hi", dir, name);
        CHECK(dir == "bin" && name == "hi");
        db_split("share/a/b", dir, name);
        CHECK(dir == "share/a" && name == "b");
        db_split("README", dir, name);
        CHECK(dir.empty() && name == "README");
    }
    {
        DbRecord r;
        r.pkg.name    = "awk";
        r.pkg.version = "1.2-r0";
        r.pkg.size    = 7;
        CHECK(r.files.push(DbFile{ "", "README", {} }));
        CHECK(r.files.push(DbFile{ "bin", "awk", {} }));

        String text;
        CHECK(db_write(r, text));
        CHECK(text.str().find("F:\nR:README\n") != Str::npos);
        CHECK(text.str().find("F:bin\nR:awk\n") != Str::npos);

        Vec<StanzaField> f;
        DbRecord back;
        CHECK(StanzaReader::one(text.str(), STANZA_DB, f));
        CHECK(db_read(f, back) == StanzaRead::Ok);
        CHECK_EQ(back.files.size(), 2);
        CHECK(back.files[0].dir.empty() && back.files[0].name == "README");
        CHECK(back.files[1].dir == "bin" && back.files[1].name == "awk");
    }
}

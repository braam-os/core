#include "cmd/pkg/trigger.h"
#include "harness.h"

namespace {

struct MatchCase {
    Str pattern;
    Str path;
    bool want;
};

// `*` covers a component and stops at the `/`, which is what lets a glob name
// the stem without naming everything under it.
constexpr MatchCase MATCHES[] = {
    { "/pkg/store/*/share", "/pkg/store/hello-1.0-r0/share", true },
    { "/pkg/store/*/share", "/pkg/store/a/b/share", false },
    { "/pkg/store/*/share/*", "/pkg/store/hello-1.0-r0/share/hello", true },
    { "/pkg/store/*/share/*", "/pkg/store/hello-1.0-r0/share", false },
    { "/pkg/bin", "/pkg/bin", true },
    { "/pkg/bin", "/pkg/bin/hi", false },
    { "/pkg", "/pkg/store", false },
    { "/pkg/*", "/pkg/store", true },
    { "/pkg/st?re/*", "/pkg/store/x", true },
    { "/pkg/[sx]tore/*", "/pkg/store/x", true },
    { "/pkg/[!s]tore/*", "/pkg/store/x", false },

    // A relative pattern cannot match an absolute path: the first component of
    // one is empty and of the other is not.
    { "pkg/store", "/pkg/store", false },
    { "/pkg/store", "pkg/store", false },
};

// One transaction's view: what it wrote, and what was already there.
constexpr TriggerDir DIRS[] = {
    { "/pkg/bin", true },
    { "/pkg/store/hello-1.0-r0", true },
    { "/pkg/store/hello-1.0-r0/share", true },
    { "/pkg/store/libz-1.0-r0/share", false },
    { "/pkg/store/libz-1.0-r0/share/libz", false },
};

bool fired(Str globs, bool fresh, Vec<Str> &out)
{
    bool fire = false;
    out.clear();
    CHECK(trigger_dirs(globs, fresh, Span<const TriggerDir>(DIRS), out, fire));
    return fire;
}

bool handed(const Vec<Str> &v, Str path)
{
    for (Str had : v)
        if (had == path)
            return true;
    return false;
}

} // namespace

void test_trigger()
{
    test_begin("trigger");

    for (const MatchCase &c : MATCHES)
        test_check(trigger_match(c.pattern, c.path) == c.want, c.pattern, __FILE_NAME__, __LINE__);

    Vec<Str> got;

    // An existing package sees only what the transaction wrote; a fresh one
    // sees everything its globs name.
    {
        CHECK(fired("/pkg/store/*/share", false, got));
        CHECK_EQ(got.size(), 1);
        CHECK(got[0] == "/pkg/store/hello-1.0-r0/share");

        CHECK(fired("/pkg/store/*/share", true, got));
        CHECK_EQ(got.size(), 2);
        CHECK(got[0] == "/pkg/store/hello-1.0-r0/share");
        CHECK(got[1] == "/pkg/store/libz-1.0-r0/share");
    }

    // Nothing matched is not a run at all.
    {
        CHECK(!fired("/pkg/store/*/lib", true, got));
        CHECK(got.empty());
        CHECK(!fired("", true, got));
    }

    // A glob that is not absolute is skipped, as apk skips one — so a package
    // whose only glob is relative never fires.
    {
        CHECK(!fired("share", true, got));
        CHECK(!fired("+share", true, got));
        CHECK(fired("share /pkg/bin", true, got));
        CHECK_EQ(got.size(), 1);
        CHECK(got[0] == "/pkg/bin");
    }

    // `+` keeps an *unmodified* directory out of argv and hands over a
    // modified one, so the same glob answers differently for the two.
    {
        CHECK(fired("+/pkg/store/*/share", true, got));
        CHECK_EQ(got.size(), 1);
        CHECK(handed(got, "/pkg/store/hello-1.0-r0/share"));
        CHECK(!handed(got, "/pkg/store/libz-1.0-r0/share"));

        // Withheld, and the trigger still runs: an argv of nothing at all.
        CHECK(fired("+/pkg/store/libz-1.0-r0/share", true, got));
        CHECK(got.empty());

        // Without the +, the same glob hands it over.
        CHECK(fired("/pkg/store/libz-1.0-r0/share", true, got));
        CHECK_EQ(got.size(), 1);
        CHECK(handed(got, "/pkg/store/libz-1.0-r0/share"));
    }

    // The first glob that matches settles a directory: whichever of the two
    // comes first decides whether libz's is withheld or handed over.
    {
        CHECK(fired("+/pkg/store/*/share /pkg/store/libz-1.0-r0/share", true, got));
        CHECK(!handed(got, "/pkg/store/libz-1.0-r0/share"));
        CHECK(fired("/pkg/store/libz-1.0-r0/share +/pkg/store/*/share", true, got));
        CHECK(handed(got, "/pkg/store/libz-1.0-r0/share"));
    }

    // §6's separators: a run of them collapses, and a newline is one.
    {
        CHECK(fired("  /pkg/bin \n /pkg/store/*/share  ", false, got));
        CHECK_EQ(got.size(), 2);
        CHECK(got[0] == "/pkg/bin" && got[1] == "/pkg/store/hello-1.0-r0/share");
    }

    // The directories arrive in the caller's order, which install.cpp sorts.
    {
        CHECK(fired("/pkg/*", false, got));
        CHECK_EQ(got.size(), 1);
        CHECK(got[0] == "/pkg/bin");
    }
}

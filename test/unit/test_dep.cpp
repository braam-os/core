#include "cmd/pkg/dep.h"
#include "harness.h"

namespace {

struct ParseCase {
    Str spec;
    DepParse how;
    Str name;
    Str version;
    u32 mask;
};

// The nine spellings, the conflict form, the names §9 stopped special-casing,
// and both ways a token can fail.
constexpr ParseCase PARSE[] = {
    { "foo", DepParse::Ok, "foo", "", VER_ANY },
    { "foo=1.2", DepParse::Ok, "foo", "1.2", VER_EQUAL },
    { "foo<1.2", DepParse::Ok, "foo", "1.2", VER_LESS },
    { "foo>1.2", DepParse::Ok, "foo", "1.2", VER_GREATER },
    { "foo<=1.2", DepParse::Ok, "foo", "1.2", VER_LESS | VER_EQUAL },
    { "foo>=1.2", DepParse::Ok, "foo", "1.2", VER_GREATER | VER_EQUAL },
    { "foo~1.2", DepParse::Ok, "foo", "1.2", VER_FUZZY | VER_EQUAL },
    { "foo<~1.2", DepParse::Ok, "foo", "1.2", VER_LESS | VER_FUZZY | VER_EQUAL },
    { "foo>~1.2", DepParse::Ok, "foo", "1.2", VER_GREATER | VER_FUZZY | VER_EQUAL },

    { "!foo", DepParse::Ok, "foo", "", VER_ANY | VER_CONFLICT },
    { "!foo=1.2", DepParse::Ok, "foo", "1.2", VER_EQUAL | VER_CONFLICT },

    // cmd: is an ordinary name (P23), @edge is part of one, and >< is LESS and
    // GREATER rather than apk's checksum comparison.
    { "cmd:awk", DepParse::Ok, "cmd:awk", "", VER_ANY },
    { "foo@edge", DepParse::Ok, "foo@edge", "", VER_ANY },
    { "foo><1.2", DepParse::Ok, "foo", "1.2", VER_LESS | VER_GREATER },

    // Named, but the version is not a version: the package is uninstallable
    // and the file still reads.
    { "foo=0.1_foobar", DepParse::Broken, "foo", "0.1_foobar", VER_EQUAL },
    { "foo>=0.1__alpha", DepParse::Broken, "foo", "0.1__alpha", VER_GREATER | VER_EQUAL },
    { "foo=0.1-r", DepParse::Broken, "foo", "0.1-r", VER_EQUAL },

    // Not a dependency at all.
    { "foo>=", DepParse::Malformed, "", "", 0 },
    { "foo~", DepParse::Malformed, "", "", 0 },
    { "=1.2", DepParse::Malformed, "", "", 0 },
    { "!", DepParse::Malformed, "", "", 0 },
    { "", DepParse::Malformed, "", "", 0 },
};

struct MatchCase {
    Str spec;
    Str version;
    bool want;
};

constexpr MatchCase MATCH[] = {
    { "foo", "1.2", true },
    { "foo", "9.9", true },

    { "foo=1.2", "1.2", true },
    { "foo=1.2", "1.3", false },
    { "foo<1.2", "1.1", true },
    { "foo<1.2", "1.2", false },
    { "foo>1.2", "1.3", true },
    { "foo>1.2", "1.2", false },
    { "foo<=1.2", "1.2", true },
    { "foo<=1.2", "1.3", false },
    { "foo>=1.2", "1.2", true },
    { "foo>=1.2", "1.1", false },

    // Fuzzy is prefix matching: the right side running out is equal (§7).
    { "foo~3.8", "3.8.1", true },
    { "foo~3.8", "3.8", true },
    { "foo~3.8", "3.6", false },
    { "foo~3.8", "3.6.9", false },
    { "foo<~3.8", "3.6", true },
    { "foo>~3.8", "3.9", true },
    { "foo>~3.8", "3.6", false },

    // A pre-release suffix makes the longer version the smaller one.
    { "foo>=1.1", "1.1_alpha1", false },
    { "foo<1.1", "1.1_alpha1", true },

    // >< is "any version but this one".
    { "foo><1.2", "1.3", true },
    { "foo><1.2", "1.2", false },

    // The conflict form answers the opposite of the same spec without its `!`.
    { "!foo", "1.2", false },
    { "!foo=1.2", "1.2", false },
    { "!foo=1.2", "1.3", true },
    { "!foo>=1.2", "1.1", true },

    // Broken satisfies nothing, whichever way it is spelled.
    { "foo=0.1_foobar", "0.1_foobar", false },
    { "!foo=0.1_foobar", "0.1_foobar", false },
};

// Runs of separators, a blank line, and one at each end.
constexpr Str LIST     = "  a b\nc\n\n  !d>=1.2   cmd:awk \n ";
constexpr Str WANTED[] = { "a", "b", "c", "!d>=1.2", "cmd:awk" };

} // namespace

void test_dep()
{
    test_begin("dep");

    for (const ParseCase &c : PARSE) {
        Dep d;
        DepParse how = dep_parse(c.spec, d);
        // The spec is the expression, so a failure names the case.
        test_check(how == c.how, c.spec, __FILE_NAME__, __LINE__);
        if (how == DepParse::Malformed)
            continue;
        test_check(d.name == c.name && d.version == c.version && d.mask == c.mask &&
                       d.broken == (c.how == DepParse::Broken),
                   c.spec, __FILE_NAME__, __LINE__);
    }

    for (const MatchCase &c : MATCH) {
        Dep d;
        DepParse how = dep_parse(c.spec, d);
        test_check(how != DepParse::Malformed, c.spec, __FILE_NAME__, __LINE__);
        test_check(dep_satisfied(d, c.version) == c.want, c.spec, __FILE_NAME__, __LINE__);
    }

    Str rest = LIST, spec;
    usize n  = 0;
    while (dep_next(rest, spec)) {
        test_check(n < sizeof(WANTED) / sizeof(WANTED[0]) && spec == WANTED[n], spec, __FILE_NAME__,
                   __LINE__);
        n++;
    }
    CHECK_EQ(n, sizeof(WANTED) / sizeof(WANTED[0]));

    Str empty = "   \n\n ", ignored;
    CHECK(!dep_next(empty, ignored));
    Str none = "";
    CHECK(!dep_next(none, ignored));
}

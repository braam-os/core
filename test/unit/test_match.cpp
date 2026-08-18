#include "cmd/sh/match.h"
#include "harness.h"
#include "kernel/str.h"

namespace {

u8 mask[64];

// The mask as text, in test_expand.cpp's notation: `^` per quoted byte and `.`
// per plain one, so a pattern and its quoting read together. An empty string
// means nothing was quoted, which is the common case.
Span<const u8> marks(Str m)
{
    usize n = 0;
    for (; n < m.size() && n < sizeof(mask); n++)
        mask[n] = m[n] == '^' ? 1 : 0;
    return Span<const u8>(mask, n);
}

bool m(Str pattern, Str mark, Str name)
{
    return glob_match(pattern, marks(mark), name);
}

bool meta(Str pattern, Str mark)
{
    return glob_meta(pattern, marks(mark));
}

} // namespace

void test_match()
{
    test_begin("match");

    // A plain byte is itself, and the name must run out with the pattern.
    CHECK(m("abc", "", "abc"));
    CHECK(!m("abc", "", "abd"));
    CHECK(!m("abc", "", "ab"));
    CHECK(!m("ab", "", "abc"));
    CHECK(m("", "", ""));
    CHECK(!m("", "", "a"));

    // `?` is one byte and never none.
    CHECK(m("a?c", "", "abc"));
    CHECK(!m("a?c", "", "ac"));
    CHECK(m("???", "", "abc"));
    CHECK(!m("?", "", ""));

    // `*` is any run, the empty one included.
    CHECK(m("*", "", ""));
    CHECK(m("*", "", "anything"));
    CHECK(m("a*", "", "a"));
    CHECK(m("a*c", "", "ac"));
    CHECK(m("a*c", "", "abbbc"));
    CHECK(!m("a*c", "", "abbbd"));
    CHECK(m("*.c", "", "main.c"));
    CHECK(!m("*.c", "", "main.h"));
    CHECK(m("*a*b*", "", "xaybz"));

    // The pattern v7's recursion is exponential on. One saved backtrack point
    // answers it without a frame.
    CHECK(m("a*a*a*b", "", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaab"));
    CHECK(!m("a*a*a*b", "", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaac"));

    // Classes: a set, a range, and both negated.
    CHECK(m("[abc]", "", "b"));
    CHECK(!m("[abc]", "", "d"));
    CHECK(m("[a-z]", "", "q"));
    CHECK(!m("[a-z]", "", "Q"));
    CHECK(m("[!a-z]", "", "Q"));
    CHECK(!m("[!a-z]", "", "q"));
    CHECK(m("f[0-9][0-9].txt", "", "f42.txt"));
    CHECK(m("[a-cx-z]", "", "y"));

    // A `-` last in the group is itself, and so is one first.
    CHECK(m("[a-]", "", "-"));
    CHECK(m("[-a]", "", "-"));

    // An unterminated class matches nothing, which is v7's answer.
    CHECK(!m("[abc", "", "a"));
    CHECK(!m("a[", "", "a["));

    // A quoted metacharacter is itself — the whole reason the mask is here.
    CHECK(!m("*", "^", "abc"));
    CHECK(m("*", "^", "*"));
    CHECK(!m("?", "^", "a"));
    CHECK(m("?", "^", "?"));
    CHECK(m("[a]", "^^^", "[a]"));
    CHECK(!m("[a]", "^^^", "a"));
    // Quoted inside a live class: the `]` closes, the `-` does not range.
    CHECK(m("[a-c]", "..^..", "-"));
    CHECK(!m("[a-c]", "..^..", "b"));

    // A mask shorter than the pattern is unmarked past its end.
    CHECK(m("a*", "^", "abc"));

    // glob_meta is what tells the walk a component needs a listing.
    CHECK(meta("*.c", ""));
    CHECK(meta("a?b", ""));
    CHECK(meta("[abc]", ""));
    CHECK(!meta("plain.c", ""));
    CHECK(!meta("*", "^"));
    CHECK(!meta("", ""));
}

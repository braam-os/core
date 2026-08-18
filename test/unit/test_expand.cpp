#include "harness.h"
#include "kernel/str.h"
#include "sh/expand.h"

namespace {

char buf[128];

// Renders what a raw word expands to: each field in braces, an error as `!`.
// With `marks`, the field is rendered as `^` per quoted byte and `.` per plain
// one instead of as its text.
Str shape(Str raw, bool marks)
{
    Vec<Field> out;
    usize n = 0;

    auto put = [&](Str s) {
        for (usize i = 0; i < s.size() && n < sizeof(buf); i++)
            buf[n++] = s[i];
    };

    if (expand_word(raw, out).is_err()) {
        put("!");
        return Str(buf, n);
    }

    for (const Field &f : out) {
        put("{");
        if (!marks)
            put(f.text.str());
        else
            for (usize i = 0; i < f.mark.size() && n < sizeof(buf); i++)
                buf[n++] = f.mark[i] ? '^' : '.';
        put("}");
    }
    return Str(buf, n);
}

Str text(Str raw)
{
    return shape(raw, false);
}

Str mark(Str raw)
{
    return shape(raw, true);
}

} // namespace

void test_expand()
{
    test_begin("expand");

    // A word with nothing to do comes through as itself, in one field.
    CHECK(text("") == "{}");
    CHECK(text("echo") == "{echo}");
    CHECK(text("a-b_c.d/e") == "{a-b_c.d/e}");

    // Quotes come off, and what they enclose survives whole.
    CHECK(text("'a b'") == "{a b}");
    CHECK(text("\"a b\"") == "{a b}");
    CHECK(text("''") == "{}");
    CHECK(text("\"\"") == "{}");

    // Quoted runs concatenate into the one word the lexer decided they were.
    CHECK(text("a''b") == "{ab}");
    CHECK(text("'a'\"b\"c") == "{abc}");
    CHECK(text("x'y z'w") == "{xy zw}");

    // A backslash outside quotes takes the next character literally.
    CHECK(text("\\|") == "{|}");
    CHECK(text("a\\ b") == "{a b}");
    CHECK(text("\\\\") == "{\\}");
    CHECK(text("\\'") == "{'}");

    // Inside double quotes only a quote and a backslash are escapable;
    // anything else keeps its backslash.
    CHECK(text("\"a\\\"b\"") == "{a\"b}");
    CHECK(text("\"a\\\\b\"") == "{a\\b}");
    CHECK(text("\"a\\nb\"") == "{a\\nb}");

    // A single quote quotes everything, a backslash included.
    CHECK(text("'a\\b'") == "{a\\b}");
    CHECK(text("'\"'") == "{\"}");
    CHECK(text("\"'\"") == "{'}");

    // The mark says where each byte came from. Nothing reads it yet.
    CHECK(mark("abc") == "{...}");
    CHECK(mark("'abc'") == "{^^^}");
    CHECK(mark("a'b'c") == "{.^.}");
    CHECK(mark("\"ab\"c") == "{^^.}");
    CHECK(mark("a\\ b") == "{.^.}");
    CHECK(mark("''") == "{}");

    // A backslash quotes one byte, so a multi-byte character behind one is
    // marked in its first byte only.
    CHECK(text("\\\xc3\xa9") == "{\xc3\xa9}");
    CHECK(mark("\\\xc3\xa9") == "{^.}");

    // The lexer would have refused all three; they are errors here too.
    CHECK(text("'a") == "!");
    CHECK(text("\"a") == "!");
    CHECK(text("a\\") == "!");
}

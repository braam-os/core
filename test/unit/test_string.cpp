#include "harness.h"
#include "kernel/str.h"
#include "kernel/string.h"

void test_string()
{
    test_begin("string");

    String s;
    CHECK(s.empty());
    CHECK_EQ(s.size(), 0);
    CHECK(s == "");

    CHECK(s.append("braam"));
    CHECK_EQ(s.size(), 5);
    CHECK(s == "braam");
    CHECK(s != "braa");
    CHECK(s.push('!'));
    CHECK(s == "braam!");
    CHECK_EQ(s[0], 'b');

    s.pop();
    CHECK(s == "braam");

    // Appending past the initial capacity reallocates and keeps the bytes.
    for (u32 i = 0; i < 100; i++)
        CHECK(s.append("xy"));
    CHECK_EQ(s.size(), 205);
    CHECK(s.capacity() >= 205);
    CHECK(s.str().starts_with("braamxy"));
    CHECK(s.str().ends_with("xy"));

    // Moving takes the buffer; the source is left empty and usable.
    String moved = move(s);
    CHECK_EQ(s.size(), 0);
    CHECK_EQ(moved.size(), 205);
    CHECK(moved.str().starts_with("braam"));
    CHECK(s.append("again"));
    CHECK(s == "again");

    String other;
    CHECK(other.assign("first"));
    CHECK(other.assign("second"));
    CHECK(other == "second");
    other = move(moved);
    CHECK_EQ(other.size(), 205);

    other.clear();
    CHECK(other.empty());
    CHECK(other == "");

    // Str sees the same bytes without copying them.
    String t;
    CHECK(t.append("hello world"));
    Str view = t;
    CHECK(view == "hello world");
    CHECK(view.data() == t.data());
}

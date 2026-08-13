#include "harness.h"
#include "kernel/str.h"

void test_str()
{
    test_begin("str");

    Str s = "hello, world";
    CHECK_EQ(s.size(), 12);
    CHECK(!s.empty());
    CHECK(Str().empty());
    CHECK_EQ(s[0], 'h');

    CHECK(s.starts_with("hello"));
    CHECK(!s.starts_with("world"));
    CHECK(s.ends_with("world"));
    CHECK(s.starts_with(""));
    CHECK(s.ends_with(s));

    CHECK_EQ(s.find(','), 5);
    CHECK_EQ(s.find('z'), Str::npos);
    CHECK_EQ(s.find("world"), 7);
    CHECK_EQ(s.find("o", 5), 8);
    CHECK(s.contains("lo, w"));
    CHECK(!s.contains("lo,  w"));

    CHECK(s.substr(7) == "world");
    CHECK(s.substr(0, 5) == "hello");
    CHECK(s.substr(99).empty());
    CHECK(s.substr(7, 99) == "world");

    Str rest;
    Str head = Str("a:b:c").split(':', rest);
    CHECK(head == "a");
    CHECK(rest == "b:c");
    head = rest.split(':', rest);
    CHECK(head == "b");
    CHECK(rest == "c");
    head = rest.split(':', rest);
    CHECK(head == "c");
    CHECK(rest.empty());

    CHECK(Str("abc") == Str("abc"));
    CHECK(Str("abc") != Str("abd"));
    CHECK(Str("abc") != Str("ab"));

    // A view is bytes, not characters: no null terminator is implied.
    CHECK_EQ(Str("a\0b", 3).size(), 3);
    CHECK_EQ("café"_s.size(), 5); // 4 characters, 5 UTF-8 bytes
}

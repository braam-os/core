#include "harness.h"

#include "user/tokenize.h"

namespace {

// Every token must be a view into the line, not a copy: the shell's argv
// borrows from a buffer that outlives it.
void check_views(Str line, const Vec<Str> &argv) {
    for (usize i = 0; i < argv.size(); i++)
        CHECK(argv[i].data() >= line.data() && argv[i].data() + argv[i].size() <= line.end());
}

} // namespace

void test_tokenize() {
    test_begin("tokenize");

    Vec<Str> argv;

    CHECK(tokenize("", argv).is_ok());
    CHECK_EQ(argv.size(), 0);

    argv.clear();
    CHECK(tokenize("   \t  ", argv).is_ok());
    CHECK_EQ(argv.size(), 0);

    argv.clear();
    CHECK(tokenize("one", argv).is_ok());
    CHECK_EQ(argv.size(), 1);
    CHECK(argv[0] == "one");

    argv.clear();
    Str line = "  echo   hello\tworld  ";
    CHECK(tokenize(line, argv).is_ok());
    CHECK_EQ(argv.size(), 3);
    CHECK(argv[0] == "echo");
    CHECK(argv[1] == "hello");
    CHECK(argv[2] == "world");
    check_views(line, argv);

    // It appends, so a caller that forgets to clear gets both lines.
    CHECK(tokenize("more", argv).is_ok());
    CHECK_EQ(argv.size(), 4);
    CHECK(argv[3] == "more");

    // No quoting yet: quotes are ordinary characters, and M4's grammar
    // introduces them along with pipes and redirection.
    argv.clear();
    CHECK(tokenize("echo 'a b'", argv).is_ok());
    CHECK_EQ(argv.size(), 3);
    CHECK(argv[1] == "'a");
    CHECK(argv[2] == "b'");
}

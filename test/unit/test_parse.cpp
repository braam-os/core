#include "harness.h"
#include "kernel/str.h"
#include "user/parse.h"

namespace {

char buf[160];

Str name_of(Redir k)
{
    switch (k) {
    case Redir::In:
        return "<";
    case Redir::Out:
        return ">";
    case Redir::Append:
        return ">>";
    case Redir::ErrOut:
        return "2>";
    case Redir::ErrAppend:
        return "2>>";
    }
    return "?";
}

// Renders a parsed pipeline: argv words in braces, redirections as operator
// plus target, stages separated by '|'.
Str shape(Str line)
{
    Pipeline pl;
    Str message;
    usize n = 0;

    auto put = [&](Str s) {
        for (usize i = 0; i < s.size() && n < sizeof(buf); i++)
            buf[n++] = s[i];
    };

    if (parse(line, pl, message).is_err()) {
        put("!");
        put(message);
        return Str(buf, n);
    }

    for (usize c = 0; c < pl.size(); c++) {
        if (c)
            put("|");
        Args a = pl.args(c);
        for (usize i = 0; i < a.size(); i++) {
            put("{");
            put(a[i]);
            put("}");
        }
        for (const Redirect &r : pl.redirects(c)) {
            put(name_of(r.kind));
            put("{");
            put(pl.target(r));
            put("}");
        }
    }
    return Str(buf, n);
}

} // namespace

void test_parse()
{
    test_begin("parse");

    // An empty line is a pipeline with no commands, not an error.
    CHECK(shape("") == "");
    CHECK(shape("   ") == "");

    CHECK(shape("echo hello") == "{echo}{hello}");
    CHECK(shape("ls | grep foo") == "{ls}|{grep}{foo}");
    CHECK(shape("ls|grep foo") == "{ls}|{grep}{foo}");

    // Redirections attach to the command they follow, not the pipeline.
    CHECK(shape("cat < in > out") == "{cat}<{in}>{out}");
    CHECK(shape("echo hi >> log") == "{echo}{hi}>>{log}");
    CHECK(shape("cmd 2> err 2>> more") == "{cmd}2>{err}2>>{more}");
    CHECK(shape("a > f | b") == "{a}>{f}|{b}");
    CHECK(shape("> f ls") == "{ls}>{f}"); // a redirection may lead

    // Quote removal survives into argv, which is the point of the owning
    // store: these words are not substrings of the line.
    CHECK(shape("echo 'a b' c") == "{echo}{a b}{c}");
    CHECK(shape("echo \"a|b\"") == "{echo}{a|b}");

    CHECK(shape("| ls") == "!syntax error near '|'");
    CHECK(shape("ls |") == "!syntax error: expected a command");
    CHECK(shape("ls | | wc") == "!syntax error near '|'");
    CHECK(shape("ls >") == "!syntax error: expected a file name");
    CHECK(shape("ls > | wc") == "!syntax error: expected a file name");
    CHECK(shape("> f") == "!syntax error: expected a command");
    CHECK(shape("echo 'a") == "!unterminated quote");
    CHECK(shape("a|b|c|d|e|f|g|h|i") == "!too many commands in a pipeline");

    // `&` ends the line, and it is the pipeline that carries the flag rather
    // than a command, since a pipeline is what runs in the background.
    {
        Pipeline pl;
        Str message;
        CHECK(parse("sleep 5 &", pl, message).is_ok());
        CHECK(pl.background());
        CHECK_EQ(pl.size(), 1);
        CHECK_EQ(pl.args(0).size(), 2);
        CHECK(pl.args(0)[1] == "5");
    }
    {
        Pipeline pl;
        Str message;
        CHECK(parse("ls | wc &", pl, message).is_ok());
        CHECK(pl.background());
        CHECK_EQ(pl.size(), 2);
    }
    {
        Pipeline pl;
        Str message;
        CHECK(parse("ls", pl, message).is_ok());
        CHECK(!pl.background());
    }
    CHECK(shape("& ls") == "!syntax error near '&'");
    CHECK(shape("ls & wc") == "!syntax error: '&' must end the line");
    CHECK(shape("echo '&'") == "{echo}{&}");

    // Stage boundaries, checked apart from the rendering above.
    {
        Pipeline pl;
        Str message;
        CHECK(parse("ls -l | grep foo | wc", pl, message).is_ok());
        CHECK_EQ(pl.size(), 3);
        CHECK_EQ(pl.args(0).size(), 2);
        CHECK(pl.args(0)[0] == "ls");
        CHECK(pl.args(0)[1] == "-l");
        CHECK_EQ(pl.args(1).size(), 2);
        CHECK(pl.args(1)[0] == "grep");
        CHECK_EQ(pl.args(2).size(), 1);
        CHECK(pl.args(2)[0] == "wc");
        CHECK(pl.frozen());
    }

    // A frozen pipeline moves without invalidating its words: String and Vec
    // move by stealing the pointer, so nothing shifts under the views.
    {
        Pipeline pl;
        Str message;
        CHECK(parse("echo 'a b' | wc", pl, message).is_ok());
        Pipeline moved = move(pl);
        CHECK_EQ(moved.size(), 2);
        CHECK(moved.args(0)[1] == "a b");
        CHECK(moved.args(1)[0] == "wc");
    }
}

#include "harness.h"
#include "kernel/str.h"
#include "sh/parse.h"

namespace {

char buf[192];

Str name_of(Redir r)
{
    switch (r) {
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

// Renders a parsed tree. A command is its assignment prefix in brackets, its
// argv in braces and its redirections as operator plus target; a pipeline is
// its stages joined by '|', with '&' after it when it is a background one; a
// list is its members joined by ';', '&&' or '||'; a group is braced; `!` is
// itself. An error is '!' and its message, and '?' when the text merely ended
// early.
struct Render {
    const Tree &t;
    usize n = 0;

    void put(Str s)
    {
        for (usize i = 0; i < s.size() && n < sizeof(buf); i++)
            buf[n++] = s[i];
    }

    void command(usize c)
    {
        Args v = t.assigns(c);
        for (usize i = 0; i < v.size(); i++) {
            put("[");
            put(v[i]);
            put("]");
        }
        Args a = t.args(c);
        for (usize i = 0; i < a.size(); i++) {
            put("{");
            put(a[i]);
            put("}");
        }
        for (const Redirect &r : t.redirects(c)) {
            put(name_of(r.kind));
            put("{");
            put(t.target(r));
            put("}");
        }
    }

    void node(u32 i)
    {
        const Tree::Node &nd = t.node(i);
        switch (nd.kind) {
        case Tree::Kind::Nop:
            return;
        case Tree::Kind::Pipe:
            for (u32 c = nd.a; c < nd.b; c++) {
                if (c != nd.a)
                    put("|");
                command(c);
            }
            if (nd.background)
                put("&");
            return;
        case Tree::Kind::Seq:
            for (u32 k = 0; k < nd.b; k++) {
                if (k)
                    put(";");
                node(t.kid(nd.a + k));
            }
            return;
        case Tree::Kind::AndOr:
            for (u32 k = 0; k < nd.b; k++) {
                if (k)
                    put(t.op(nd.c + k - 1) == Tree::Op::And ? "&&" : "||");
                node(t.kid(nd.a + k));
            }
            return;
        case Tree::Kind::Not:
            put("!");
            node(nd.a);
            return;
        case Tree::Kind::Group:
            put("{");
            node(nd.a);
            put("}");
            return;
        }
    }
};

Str shape(Str line)
{
    Tree t;
    ParseErr err;
    Render r{ t };

    if (parse(line, t, err).is_err()) {
        r.put(err.more ? "?" : "!");
        r.put(err.message);
        return Str(buf, r.n);
    }
    r.node(t.root());
    return Str(buf, r.n);
}

} // namespace

void test_parse()
{
    test_begin("parse");

    // An empty text is a tree whose root is nothing, not an error.
    CHECK(shape("") == "");
    CHECK(shape("   ") == "");
    CHECK(shape("\n\n") == "");
    CHECK(shape("# just a comment") == "");

    CHECK(shape("echo hello") == "{echo}{hello}");
    CHECK(shape("ls | grep foo") == "{ls}|{grep}{foo}");
    CHECK(shape("ls|grep foo") == "{ls}|{grep}{foo}");

    // Redirections attach to the command they follow, not the pipeline.
    CHECK(shape("cat < in > out") == "{cat}<{in}>{out}");
    CHECK(shape("echo hi >> log") == "{echo}{hi}>>{log}");
    CHECK(shape("cmd 2> err 2>> more") == "{cmd}2>{err}2>>{more}");
    CHECK(shape("a > f | b") == "{a}>{f}|{b}");
    CHECK(shape("> f ls") == "{ls}>{f}"); // a redirection may lead

    // A word is stored raw: quoting decides where it ends, and what it then
    // means is expand.h's.
    CHECK(shape("echo 'a b' c") == "{echo}{'a b'}{c}");
    CHECK(shape("echo \"a|b\"") == "{echo}{\"a|b\"}");

    // An assignment is a word the parser knows: leading, and on the raw text,
    // so a name is what a `$` would take.
    CHECK(shape("x=1 ls") == "[x=1]{ls}");
    CHECK(shape("x=1 y=2 ls a") == "[x=1][y=2]{ls}{a}");
    CHECK(shape("x=1") == "[x=1]");
    CHECK(shape("x= ls") == "[x=]{ls}");
    CHECK(shape("a2=1 ls") == "[a2=1]{ls}");
    CHECK(shape("_x=1 ls") == "[_x=1]{ls}");
    CHECK(shape("ls x=1") == "{ls}{x=1}");   // not leading
    CHECK(shape("2a=1 ls") == "{2a=1}{ls}"); // not a name
    CHECK(shape("=1 ls") == "{=1}{ls}");
    CHECK(shape("x=1 | y=2") == "[x=1]|[y=2]");
    CHECK(shape("x=1 > f") == "[x=1]>{f}");

    // ---- lists ----

    CHECK(shape("a; b") == "{a};{b}");
    CHECK(shape("a;b;c") == "{a};{b};{c}");
    CHECK(shape("a\nb") == "{a};{b}");
    CHECK(shape("a;") == "{a}");
    CHECK(shape(";;a") == "{a}");
    CHECK(shape("a && b") == "{a}&&{b}");
    CHECK(shape("a || b") == "{a}||{b}");
    CHECK(shape("! a") == "!{a}");
    CHECK(shape("! a | b") == "!{a}|{b}");

    // `&&` and `||` are one left-to-right chain rather than a nest, which is
    // what keeps the walk's depth the text's nesting depth.
    CHECK(shape("a && b || c") == "{a}&&{b}||{c}");
    CHECK(shape("a || b && c && d") == "{a}||{b}&&{c}&&{d}");
    CHECK(shape("a; b && c") == "{a};{b}&&{c}");
    CHECK(shape("a | b && c | d") == "{a}|{b}&&{c}|{d}");
    CHECK(shape("a &&\nb") == "{a}&&{b}"); // a newline after the operator

    // `&` is a separator now, and it marks the pipeline it follows.
    CHECK(shape("sleep 5 &") == "{sleep}{5}&");
    CHECK(shape("ls | wc &") == "{ls}|{wc}&");
    CHECK(shape("a & b") == "{a}&;{b}");
    CHECK(shape("a & b &") == "{a}&;{b}&");
    CHECK(shape("ls") == "{ls}");

    // A group, which must have a separator before its `}` — `{ a }` passes
    // `}` as an argument, which is what Bourne does.
    CHECK(shape("{ a; b; }") == "{{a};{b}}");
    CHECK(shape("{ a; }") == "{{a}}");
    CHECK(shape("{ a\nb\n}") == "{{a};{b}}");
    CHECK(shape("{ a }") == "?syntax error: expected '}'"); // `}` became an argument
    CHECK(shape("! { a; }") == "!{{a}}");
    CHECK(shape("{ a; } && b") == "{{a}}&&{b}");
    CHECK(shape("{ { a; }; }") == "{{{a}}}");
    CHECK(shape("echo '{'") == "{echo}{'{'}"); // quoted, so not reserved

    // ---- refusals ----

    CHECK(shape("| ls") == "!syntax error near '|'");
    CHECK(shape("&& ls") == "!syntax error near '&&'");
    CHECK(shape("& ls") == "!syntax error near '&'");
    CHECK(shape("ls | | wc") == "!syntax error near '|'");
    CHECK(shape("ls >") == "!syntax error: expected a file name");
    CHECK(shape("ls > | wc") == "!syntax error: expected a file name");
    CHECK(shape("> f") == "!syntax error: expected a command");
    CHECK(shape("a|b|c|d|e|f|g|h|i") == "!too many commands in a pipeline");
    CHECK(shape("}") == "!syntax error near '}'");

    // Backgrounding is for a pipeline: the shell keeps running while it goes,
    // and nothing in a process can wait for a sibling task.
    CHECK(shape("a && b &") == "!cannot run a list in the background");
    CHECK(shape("{ a; } &") == "!cannot run a list in the background");

    // A group needs the base stdio S7 threads through, so it may not yet be
    // piped or redirected.
    CHECK(shape("{ a; } | wc") == "!syntax error: a group cannot be piped or redirected yet");
    CHECK(shape("{ a; } > f") == "!syntax error: a group cannot be piped or redirected yet");

    // ---- `more`: the text ended inside something ----

    CHECK(shape("ls |") == "?syntax error: expected a command");
    CHECK(shape("a &&") == "?syntax error: expected a command");
    CHECK(shape("a ||") == "?syntax error: expected a command");
    CHECK(shape("{ a;") == "?syntax error: expected '}'");
    CHECK(shape("{ a; b") == "?syntax error: expected '}'");
    CHECK(shape("echo 'a") == "?unterminated quote or ${");
    CHECK(shape("echo ${x") == "?unterminated quote or ${");
    CHECK(shape("!") == "!syntax error: expected a command");

    // Stage boundaries, checked apart from the rendering above.
    {
        Tree t;
        ParseErr err;
        CHECK(parse("ls -l | grep foo | wc", t, err).is_ok());
        const Tree::Node &n = t.node(t.root());
        CHECK(n.kind == Tree::Kind::Pipe);
        CHECK_EQ(n.b - n.a, 3);
        CHECK_EQ(t.args(0).size(), 2);
        CHECK(t.args(0)[0] == "ls");
        CHECK(t.args(0)[1] == "-l");
        CHECK(t.args(1)[0] == "grep");
        CHECK(t.args(2)[0] == "wc");
        CHECK(t.frozen());
    }

    // The bytes a pipeline came from, which is what `jobs` lists — its own,
    // not the whole line's.
    {
        Tree t;
        ParseErr err;
        CHECK(parse("echo hi; sleep 5 &", t, err).is_ok());
        const Tree::Node &root = t.node(t.root());
        CHECK(root.kind == Tree::Kind::Seq);
        CHECK_EQ(root.b, 2);
        CHECK(t.text(t.kid(root.a)) == "echo hi");
        CHECK(t.text(t.kid(root.a + 1)) == "sleep 5");
        CHECK(t.node(t.kid(root.a + 1)).background);
    }

    // A frozen tree moves without invalidating its words: String and Vec move
    // by stealing the pointer, so nothing shifts under the views.
    {
        Tree t;
        ParseErr err;
        CHECK(parse("echo 'a b' | wc", t, err).is_ok());
        Tree moved = move(t);
        CHECK_EQ(moved.size(), 2);
        CHECK(moved.args(0)[1] == "'a b'");
        CHECK(moved.args(1)[0] == "wc");
    }
}

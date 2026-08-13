#include "harness.h"
#include "kernel/str.h"
#include "user/tokenize.h"

namespace {

// Runs the lexer over a line and renders the token stream: words appear in
// braces, operators as themselves, so one comparison checks both.
Str lex(Str line, char *out, usize cap)
{
    Lexer lx(line);
    String w;
    usize n = 0;

    auto put = [&](Str s) {
        for (usize i = 0; i < s.size() && n < cap; i++)
            out[n++] = s[i];
    };

    for (;;) {
        Result<Tok> t = lx.next(w);
        if (t.is_err()) {
            put("!");
            break;
        }
        switch (t.value()) {
        case Tok::End:
            return Str(out, n);
        case Tok::Word:
            put("{");
            put(w.str());
            put("}");
            break;
        case Tok::Pipe:
            put("|");
            break;
        case Tok::Less:
            put("<");
            break;
        case Tok::Great:
            put(">");
            break;
        case Tok::DGreat:
            put(">>");
            break;
        case Tok::ErrGreat:
            put("2>");
            break;
        case Tok::ErrDGreat:
            put("2>>");
            break;
        }
    }
    return Str(out, n);
}

char buf[128];

Str lexed(Str line)
{
    return lex(line, buf, sizeof(buf));
}

} // namespace

void test_tokenize()
{
    test_begin("tokenize");

    CHECK(lexed("") == "");
    CHECK(lexed("   ") == "");
    CHECK(lexed("echo") == "{echo}");
    CHECK(lexed("  echo   hello  world ") == "{echo}{hello}{world}");
    CHECK(lexed("a\tb") == "{a}{b}");

    // Quotes come off, and what they enclose survives whole.
    CHECK(lexed("echo 'a b'") == "{echo}{a b}");
    CHECK(lexed("echo \"a b\"") == "{echo}{a b}");
    CHECK(lexed("echo ''") == "{echo}{}");
    CHECK(lexed("a''b") == "{ab}");
    CHECK(lexed("'a'\"b\"c") == "{abc}");

    // An operator inside quotes, or behind a backslash, is a character.
    CHECK(lexed("echo '|'") == "{echo}{|}");
    CHECK(lexed("echo \\|") == "{echo}{|}");
    CHECK(lexed("echo a\\ b") == "{echo}{a b}");

    // Inside double quotes only a quote and a backslash are escapable.
    CHECK(lexed("\"a\\\"b\"") == "{a\"b}");
    CHECK(lexed("\"a\\\\b\"") == "{a\\b}");
    CHECK(lexed("\"a\\nb\"") == "{a\\nb}");

    // Operators split words without needing spaces around them.
    CHECK(lexed("a|b") == "{a}|{b}");
    CHECK(lexed("ls | grep foo") == "{ls}|{grep}{foo}");
    CHECK(lexed("a>b") == "{a}>{b}");
    CHECK(lexed("a>>b") == "{a}>>{b}");
    CHECK(lexed("a<b") == "{a}<{b}");

    // A file descriptor binds only as a whole prefix.
    CHECK(lexed("cmd 2>f") == "{cmd}2>{f}");
    CHECK(lexed("cmd 2>>f") == "{cmd}2>>{f}");
    CHECK(lexed("cmd a2>f") == "{cmd}{a2}>{f}");
    CHECK(lexed("echo 2") == "{echo}{2}");

    // An unclosed quote, or a line ending in a backslash, is an error.
    CHECK(lexed("echo 'a") == "{echo}!");
    CHECK(lexed("echo \"a") == "{echo}!");
    CHECK(lexed("echo a\\") == "{echo}!");
}

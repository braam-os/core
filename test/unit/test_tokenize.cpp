#include "cmd/sh/tokenize.h"
#include "harness.h"
#include "kernel/str.h"

namespace {

// Runs the lexer over a line and renders the token stream: words appear in
// braces, operators as themselves, so one comparison checks both. A word is
// raw — its quotes are still in it, and test_expand.cpp takes them off.
Str lex(Str line, char *out, usize cap)
{
    Lexer lx(line);
    Str w;
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
            put(w);
            put("}");
            break;
        case Tok::Newline:
            put("\\n");
            break;
        case Tok::Semi:
            put(";");
            break;
        case Tok::DSemi:
            put(";;");
            break;
        case Tok::LParen:
            put("(");
            break;
        case Tok::RParen:
            put(")");
            break;
        case Tok::Amp:
            put("&");
            break;
        case Tok::AndIf:
            put("&&");
            break;
        case Tok::OrIf:
            put("||");
            break;
        case Tok::Pipe:
            put("|");
            break;
        case Tok::Less:
            put("<");
            break;
        case Tok::DLess:
            put("<<");
            break;
        case Tok::DLessDash:
            put("<<-");
            break;
        case Tok::GreatAnd:
            put(">&");
            break;
        case Tok::ErrGreatAnd:
            put("2>&");
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

    // The quotes stay in: what the lexer decides is where a word ends.
    CHECK(lexed("echo 'a b'") == "{echo}{'a b'}");
    CHECK(lexed("echo \"a b\"") == "{echo}{\"a b\"}");
    CHECK(lexed("echo ''") == "{echo}{''}");
    CHECK(lexed("a''b") == "{a''b}");
    CHECK(lexed("'a'\"b\"c") == "{'a'\"b\"c}");

    // An operator inside quotes, or behind a backslash, is a character.
    CHECK(lexed("echo '|'") == "{echo}{'|'}");
    CHECK(lexed("echo \\|") == "{echo}{\\|}");
    CHECK(lexed("echo a\\ b") == "{echo}{a\\ b}");

    // Inside double quotes only a quote and a backslash are escapable, which
    // the lexer must know or a `\"` would close the run early.
    CHECK(lexed("\"a\\\"b\"") == "{\"a\\\"b\"}");
    CHECK(lexed("\"a\\\\b\"") == "{\"a\\\\b\"}");
    CHECK(lexed("\"a\\nb\"") == "{\"a\\nb\"}");

    // Operators split words without needing spaces around them.
    // The list operators, and the two-character scan that tells them apart.
    CHECK(lexed("a;b") == "{a};{b}");
    CHECK(lexed("a && b") == "{a}&&{b}");
    CHECK(lexed("a||b") == "{a}||{b}");
    CHECK(lexed("a & b") == "{a}&{b}");
    CHECK(lexed("a\nb") == "{a}\\n{b}");
    CHECK(lexed("a\n\nb") == "{a}\\n\\n{b}");

    // A comment only where a token could start.
    CHECK(lexed("a # b") == "{a}");
    CHECK(lexed("a#b") == "{a#b}");
    CHECK(lexed("# all of it") == "");
    CHECK(lexed("a # b\nc") == "{a}\\n{c}");
    CHECK(lexed("echo '#'") == "{echo}{'#'}");

    CHECK(lexed("a|b") == "{a}|{b}");
    CHECK(lexed("ls | grep foo") == "{ls}|{grep}{foo}");
    CHECK(lexed("a>b") == "{a}>{b}");
    CHECK(lexed("a>>b") == "{a}>>{b}");
    CHECK(lexed("a<b") == "{a}<{b}");
    CHECK(lexed("sleep 5&") == "{sleep}{5}&");
    CHECK(lexed("echo '&'") == "{echo}{'&'}");

    // A file descriptor binds only as a whole prefix.
    CHECK(lexed("cmd 2>f") == "{cmd}2>{f}");
    CHECK(lexed("cmd 2>>f") == "{cmd}2>>{f}");
    CHECK(lexed("cmd a2>f") == "{cmd}{a2}>{f}");
    CHECK(lexed("echo 2") == "{echo}{2}");

    // An unclosed quote, or a line ending in a backslash, is an error.
    // A `${…}` is one piece of the word: what is in it is the expander's, so a
    // separator in there does not end the word.
    CHECK(lexed("echo ${x}") == "{echo}{${x}}");
    CHECK(lexed("echo ${x-a b}") == "{echo}{${x-a b}}");
    CHECK(lexed("echo ${x-a|b}") == "{echo}{${x-a|b}}");
    CHECK(lexed("echo ${x-${y}}z") == "{echo}{${x-${y}}z}");
    CHECK(lexed("echo ${x-'}'}") == "{echo}{${x-'}'}}");
    CHECK(lexed("echo $x|b") == "{echo}{$x}|{b}");

    // A `$(…)` and a backtick run are one piece of the word for the same
    // reason: what is in one is a command, so a separator there does not end
    // the word. The parens are operators (S4) only when they stand alone.
    CHECK(lexed("echo $(a b)") == "{echo}{$(a b)}");
    CHECK(lexed("echo $(a|b)") == "{echo}{$(a|b)}");
    CHECK(lexed("echo $(a; b)") == "{echo}{$(a; b)}");
    CHECK(lexed("echo $(a $(b))c") == "{echo}{$(a $(b))c}");
    CHECK(lexed("echo $(a ')' b)") == "{echo}{$(a ')' b)}");
    CHECK(lexed("echo x$(a)y") == "{echo}{x$(a)y}");
    CHECK(lexed("echo `a b`") == "{echo}{`a b`}");
    CHECK(lexed("echo \"`a b`\"") == "{echo}{\"`a b`\"}");
    CHECK(lexed("echo (x)") == "{echo}({x})"); // still operators on their own

    CHECK(lexed("echo 'a") == "{echo}!");
    CHECK(lexed("echo \"a") == "{echo}!");
    CHECK(lexed("echo a\\") == "{echo}!");
    CHECK(lexed("echo ${x") == "{echo}!");
    CHECK(lexed("echo $(a") == "{echo}!");
    CHECK(lexed("echo `a") == "{echo}!");
}

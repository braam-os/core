#include "cmd/sh/cond.h"
#include "harness.h"
#include "kernel/str.h"
#include "kernel/vec.h"

namespace {

// What a filesystem would answer, as a literal: `yes` exists and is a file,
// `dir` is a directory, `1` is the terminal, and nothing else is anything.
// This is the whole reason cond.cpp takes its answers rather than fetching
// them — the grammar is testable with no store under it.
u8 answer_of(const CondProbe &p)
{
    if (p.op == 'd')
        return p.arg == "dir" ? 1 : 0;
    if (p.op == 't')
        return p.arg == "1" ? 1 : 0;
    // The two binary primaries: `new` is the younger of the pair.
    if (p.op == 'n')
        return p.arg == "new" && p.arg2 != "new" ? 1 : 0;
    if (p.op == 'o')
        return p.arg != "new" && p.arg2 == "new" ? 1 : 0;
    return p.arg == "yes" ? 1 : 0;
}

Str words[16];

// The expression as it would be typed, split on spaces. `''` is the empty word,
// which is what a shell hands over for an unset variable in quotes.
Args split(Str line)
{
    usize n = 0;
    for (usize i = 0; i < line.size() && n < 16;) {
        while (i < line.size() && line[i] == ' ')
            i++;
        usize from = i;
        while (i < line.size() && line[i] != ' ')
            i++;
        if (i == from)
            break;
        Str w = line.substr(from, i - from);
        if (w == "''")
            w = Str();
        words[n++] = w;
    }
    return Args{ Span<const Str>(words, n) };
}

// "T", "F", or "!" and why. One string compare checks a whole expression, in
// test_parse.cpp's style.
Str shape(Str line)
{
    Args a = split(line);

    Vec<CondProbe> probes;
    if (!cond_probes(a, probes))
        return "!out of memory";

    Vec<u8> answers;
    for (const CondProbe &p : probes)
        if (!answers.push(answer_of(p)))
            return "!out of memory";

    bool value = false;
    CondErr err;
    if (cond_eval(a, Span<const u8>(answers.data(), answers.size()), value, err))
        return value ? "T" : "F";

    static char buf[64];
    buf[0]  = '!';
    usize n = err.message.size() < sizeof(buf) - 1 ? err.message.size() : sizeof(buf) - 1;
    for (usize i = 0; i < n; i++)
        buf[i + 1] = err.message[i];
    return Str(buf, n + 1);
}

} // namespace

void test_cond()
{
    test_begin("cond");

    // No expression is false, and one word is true when it is not empty.
    CHECK(shape("") == "F");
    CHECK(shape("x") == "T");
    CHECK(shape("''") == "F");

    // ---- strings ----

    CHECK(shape("-n x") == "T");
    CHECK(shape("-n ''") == "F");
    CHECK(shape("-z ''") == "T");
    CHECK(shape("-z x") == "F");
    CHECK(shape("a = a") == "T");
    CHECK(shape("a = b") == "F");
    CHECK(shape("a != b") == "T");

    // ---- numbers ----

    CHECK(shape("1 -eq 1") == "T");
    CHECK(shape("1 -ne 1") == "F");
    CHECK(shape("2 -gt 1") == "T");
    CHECK(shape("2 -ge 2") == "T");
    CHECK(shape("1 -lt 2") == "T");
    CHECK(shape("1 -le 0") == "F");
    CHECK(shape("-1 -lt 0") == "T");
    // v7's atoi, not parse_u32: a word that is not a number is zero.
    CHECK(shape("x -eq 0") == "T");
    CHECK(shape("12x -eq 12") == "T");

    // ---- the file primaries, answered from the table above ----

    CHECK(shape("-f yes") == "T");
    CHECK(shape("-f no") == "F");
    CHECK(shape("-d dir") == "T");
    CHECK(shape("-d yes") == "F");
    CHECK(shape("-r yes") == "T");
    CHECK(shape("-w yes") == "T");
    CHECK(shape("-x yes") == "T");
    CHECK(shape("-s yes") == "T");
    CHECK(shape("-t") == "T"); // no operand is descriptor 1
    CHECK(shape("-t 2") == "F");

    // The binary pair, which reads its two words as one probe.
    CHECK(shape("new -nt old") == "T");
    CHECK(shape("old -nt new") == "F");
    CHECK(shape("old -ot new") == "T");
    CHECK(shape("new -ot old") == "F");
    CHECK(shape("new -nt old -a new -nt old") == "T");
    CHECK(shape("! new -nt old") == "F");
    CHECK(shape("new -nt") == "!argument expected");

    // ---- precedence ----

    CHECK(shape("! -f yes") == "F");
    CHECK(shape("! -f no") == "T");
    CHECK(shape("-f yes -a -f yes") == "T");
    CHECK(shape("-f yes -a -f no") == "F");
    CHECK(shape("-f no -o -f yes") == "T");
    CHECK(shape("-f no -o -f no") == "F");
    // `-a` binds tighter than `-o`, as it does in v7.
    CHECK(shape("-f no -a -f no -o -f yes") == "T");
    CHECK(shape("( -f no -o -f yes )") == "T");
    CHECK(shape("! ( -f yes -a -f yes )") == "F");

    // Every primary is probed even where the answer could not change the
    // result: v7's `-a` and `-o` are the bitwise `&` and `|`, and that is what
    // lets the awaits be lifted out of the recursion.
    CHECK(shape("-f no -a -d dir") == "F");

    // ---- what is neither true nor false ----

    CHECK(shape("-f") == "!argument expected");
    CHECK(shape("( -f yes") == "!argument expected");
    CHECK(shape("( -f yes -f no") == "!) expected");
    CHECK(shape("a b c") == "!unknown operator");
    CHECK(shape("!") == "!argument expected");
}

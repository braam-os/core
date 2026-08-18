#include "cond.h"

#include "kernel/text.h"

namespace {

// v7's atoi, which is not parse_u32: it takes a sign, stops at the first byte
// that is not a digit, and answers 0 for a word that is not a number at all.
// `test x -eq 0` is true there, and this is why.
i32 to_int(Str s)
{
    usize i  = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+'))
        neg = s[i++] == '-';

    i32 v = 0;
    for (; i < s.size() && is_digit(s[i]); i++)
        v = v * 10 + (s[i] - '0');
    return neg ? -v : v;
}

// One walk of the expression. Collecting and evaluating are the same walk with
// `probes` set or not: the grammar consumes argv strictly left to right and
// nothing in it short-circuits, so both passes read the same tokens in the
// same order and the probe indices line up by construction.
struct Walk {
    Walk(Args v, Span<const u8> got, Vec<CondProbe> *want) : a(v), answers(got), probes(want) {}

    Args a;
    Span<const u8> answers;
    Vec<CondProbe> *probes = nullptr;

    usize at    = 0; // v7's `ap`
    usize probe = 0;
    u32 depth   = 0;
    bool bad    = false;
    bool oom    = false;
    Str message;

    // Same nesting bound as the parser's (parse.h): this recursion is on the
    // wasm stack too.
    static constexpr u32 MAX_DEPTH = 16;

    void fail(Str why)
    {
        if (!bad) {
            bad     = true;
            message = why;
        }
    }

    // nxtarg(0): an argument is required.
    Str need()
    {
        if (at >= a.size()) {
            fail("argument expected");
            return Str();
        }
        return a[at++];
    }

    // nxtarg(1): running off the end is an answer rather than an error, which
    // is how a trailing word tells "nothing follows" from "something
    // unexpected follows". It steps past the end anyway, because back() puts
    // it back either way.
    bool opt(Str &out)
    {
        at++;
        if (at > a.size())
            return false;
        out = a[at - 1];
        return true;
    }

    void back()
    {
        if (at)
            at--;
    }

    bool ask(char op, Str arg)
    {
        if (probes) {
            if (!probes->push(CondProbe{ op, arg }))
                oom = true;
            return false;
        }
        usize i = probe++;
        return i < answers.size() && answers[i] != 0;
    }

    // expression -o expression
    bool e0()
    {
        bool p1 = e1();
        Str t;
        if (opt(t) && t == "-o") {
            // `|`, not `||`: v7 evaluates both sides whatever the left one
            // said, which is what lets every primary be probed up front.
            bool p2 = e0();
            return p1 | p2;
        }
        back();
        return p1;
    }

    // expression -a expression
    bool e1()
    {
        bool p1 = e2();
        Str t;
        if (opt(t) && t == "-a") {
            bool p2 = e1();
            return p1 & p2;
        }
        back();
        return p1;
    }

    // ! expression
    bool e2()
    {
        Str t = need();
        if (bad)
            return false;
        if (t == "!")
            return !e3();
        back();
        return e3();
    }

    // The primaries, and the parenthesised sub-expression.
    bool e3()
    {
        if (bad)
            return false;
        if (depth >= MAX_DEPTH) {
            fail("too deeply nested");
            return false;
        }
        depth++;
        bool v = primary();
        depth--;
        return v;
    }

    bool primary()
    {
        Str a0 = need();
        if (bad)
            return false;

        if (a0 == "(") {
            bool p1 = e0();
            Str t   = need();
            if (!bad && t != ")")
                fail(") expected");
            return p1;
        }

        // The file primaries, whose answer somebody else went and got. The
        // letter is the operator's second byte.
        if (a0 == "-r" || a0 == "-w" || a0 == "-x" || a0 == "-d" || a0 == "-f" || a0 == "-s") {
            Str f = need();
            if (bad)
                return false;
            return ask(a0[1], f);
        }
        if (a0 == "-t") {
            // A literal per branch: Str's length folds only when the pointer
            // is constant (Concept.md §C.3).
            if (at >= a.size())
                return ask('t', "1");
            Str f = need();
            return bad ? false : ask('t', f);
        }

        if (a0 == "-n")
            return !need().empty();
        if (a0 == "-z")
            return need().empty();

        Str p2;
        if (!opt(p2)) {
            back();
            return !a0.empty(); // one word: true when it is not empty
        }

        if (p2 == "=")
            return need() == a0;
        if (p2 == "!=")
            return need() != a0;

        i32 n1 = to_int(a0);
        i32 n2 = to_int(need());
        if (bad)
            return false;
        if (p2 == "-eq")
            return n1 == n2;
        if (p2 == "-ne")
            return n1 != n2;
        if (p2 == "-gt")
            return n1 > n2;
        if (p2 == "-ge")
            return n1 >= n2;
        if (p2 == "-lt")
            return n1 < n2;
        if (p2 == "-le")
            return n1 <= n2;

        fail("unknown operator");
        return false;
    }
};

} // namespace

bool cond_probes(Args a, Vec<CondProbe> &out)
{
    if (a.size() == 0)
        return true;

    Walk w(a, Span<const u8>(), &out);
    w.e0();
    return !w.oom;
}

bool cond_eval(Args a, Span<const u8> answers, bool &value, CondErr &err)
{
    // No expression is false, as v7's `ac <= 1` is.
    if (a.size() == 0) {
        value = false;
        return true;
    }

    Walk w(a, answers, nullptr);
    bool v = w.e0();
    if (w.bad) {
        err.message = w.message;
        return false;
    }
    value = v;
    return true;
}

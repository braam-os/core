#include "cmd/pkg/version.h"
#include "harness.h"

namespace {

// apk-tools/test/unit/version.data, verbatim inside a two-line raw-string
// wrapper, and version_test.c's loop over it.
constexpr Str DATA =
#include "version.data"
    ;

constexpr usize CASES = 785;

bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

// Trailing only, which is apk_blob_trim.
Str trim_end(Str s)
{
    while (!s.empty() && is_ws(s[s.size() - 1]))
        s = s.substr(0, s.size() - 1);
    return s;
}

bool next_line(Str &rest, Str &line)
{
    if (rest.empty())
        return false;
    usize at = rest.find('\n');
    if (at == Str::npos) {
        line = rest;
        rest = Str();
    } else {
        line = rest.substr(0, at);
        rest = rest.substr(at + 1);
    }
    return true;
}

// True when a space split it, with `head` before and `rest` after.
bool split(Str s, Str &head, Str &rest)
{
    usize at = s.find(' ');
    if (at == Str::npos)
        return false;
    head = s.substr(0, at);
    rest = s.substr(at + 1);
    return true;
}

bool pull_bang(Str &s)
{
    if (!s.starts_with("!"))
        return false;
    s = s.substr(1);
    return true;
}

} // namespace

void test_version()
{
    test_begin("version");

    usize cases = 0;
    Str rest    = DATA, line;
    while (next_line(rest, line)) {
        usize hash = line.find('#');
        Str arg    = trim_end(hash == Str::npos ? line : line.substr(0, hash));
        if (arg.empty())
            continue;
        cases++;

        Str ver1, op, ver2, tail;
        bool ok, invert;
        if (split(arg, ver1, tail) && split(tail, op, ver2)) {
            invert = pull_bang(op);
            ok     = version_match(ver1, version_mask(op), ver2);
        } else {
            ver1   = arg;
            invert = pull_bang(ver1);
            ok     = version_valid(ver1);
        }
        // The line itself is the expression, so a failure names the case.
        test_check(invert ? !ok : ok, arg, __FILE_NAME__, __LINE__);
    }

    CHECK_EQ(cases, CASES);
}

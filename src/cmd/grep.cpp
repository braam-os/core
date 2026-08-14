#include "proc/io.h"

namespace {

char fold_case(char c)
{
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
}

// Plain substring search. There is no regular-expression engine, and the usage
// line says so rather than implying one.
bool contains(Str hay, Str needle, bool fold)
{
    if (needle.size() > hay.size())
        return false;
    for (usize i = 0; i + needle.size() <= hay.size(); i++) {
        usize j = 0;
        while (j < needle.size()) {
            char a = hay[i + j], b = needle[j];
            if (fold) {
                a = fold_case(a);
                b = fold_case(b);
            }
            if (a != b)
                break;
            j++;
        }
        if (j == needle.size())
            return true;
    }
    return false;
}

} // namespace

Task<i32> proc_main(Args args)
{
    bool invert = false, fold = false;
    usize i = 1;
    for (; i < args.size(); i++) {
        if (args[i] == "-v")
            invert = true;
        else if (args[i] == "-i")
            fold = true;
        else
            break;
    }
    if (i >= args.size()) {
        co_await write_all(SYS_STDERR, "usage: grep [-i] [-v] <text> [<file>...]\n");
        co_return 2;
    }

    Str pattern = args[i];
    Input files(Args{ args.v.subspan(i + 1) }, SYS_STDIN);
    if (i32 bad = co_await files.open_all("grep"))
        co_return bad;

    LineReader in(files);
    String line;
    bool matched = false;

    for (;;) {
        Result<bool> r = co_await in.next(line);
        if (r.is_err())
            co_return r.error() == Error::Cancelled ? 130 : 1;
        if (!r.value())
            break;
        if (contains(line.str(), pattern, fold) == invert)
            continue;

        matched = true;
        if (!line.push('\n'))
            co_return 1;
        if ((co_await write_all(SYS_STDOUT, line.str())).is_err())
            co_return 1;
    }

    co_return matched ? 0 : 1;
}

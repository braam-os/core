#include "pkg.h"

#include "db.h"
#include "kernel/fmt.h"
#include "kernel/traits.h"
#include "proc/io.h"
#include "store.h"

namespace {

constexpr Str NO_INDEX = "pkg: no index; run pkg update\n";

// Explicit: --gc-sections never extracts an unreferenced archive member. In
// /etc/help's order, which is what a reader does with them rather than the
// alphabet; find() is a scan, so nothing here depends on the order.
constexpr PkgCommand TABLE[] = {
    { "update", "", "fetch the repository index", pkg_update },
    { "search", "<pattern>", "search names and descriptions", pkg_search },
    { "info", "<package>", "what the index says about one", pkg_info },
    { "install", "<package>...", "install packages and dependencies", pkg_install },
    { "remove", "<package>...", "remove packages", pkg_remove },
    { "autoremove", "", "remove dependencies nothing needs", pkg_autoremove },
    { "upgrade", "", "upgrade everything installed", pkg_upgrade },
    { "list", "", "list installed packages", pkg_list },
    { "files", "<package>", "the files a package installed", pkg_files },
    { "verify", "[<package>]", "re-check installed files", pkg_verify },
    { "clean", "", "drop archives nothing needs", pkg_clean },
    { "help", "", "print this message", pkg_help },
};

// One option is not a table: the row is written out, aligned by hand to the
// column the commands compute below.
constexpr Str HEAD = "Usage:\n"
                     "    pkg [-v] <command> [<arg>...]\n"
                     "Options:\n"
                     "    -v, --verbose         trace each HTTP request and reply\n"
                     "Commands:\n";
constexpr Str USAGE_HELP = "Usage: pkg help\n";
constexpr Str NO_MEMORY  = "pkg: out of memory\n";
constexpr usize GAP      = 2;

// The two a bare Error cannot say, in curl.cpp's words: web/svc.js reaches Perm
// through a no-cors retry the origin answered, and Io when nothing did.
constexpr Str CROSS_ORIGIN =
    "pkg: response is not accessible because the server did not grant "
    "cross-origin access\n";
constexpr Str NO_ANSWER = "pkg: no answer\n";

bool verbose_on = false;

// The words with the flags taken out. Err(Invalid) is a word beginning `-`
// that is none of them, with `bad` naming it; a §6 token cannot begin with one,
// so nothing an operand may be is lost.
Result<void> take_flags(Args in, Vec<Str> &out, Str &bad)
{
    for (usize i = 0; i < in.size(); i++) {
        Str w = in[i];
        if (w == "-v" || w == "--verbose") {
            pkg_set_verbose(true);
            continue;
        }
        if (w.size() > 1 && w[0] == '-' && w != "-h" && w != "--help") {
            bad = w;
            return Err(Error::Invalid);
        }
        if (!out.push(w))
            return Err(Error::NoMemory);
    }
    return Result<void>();
}

const PkgCommand *find(Str name)
{
    for (const PkgCommand &c : TABLE)
        if (c.name == name)
            return &c;
    return nullptr;
}

// `name args`, which is the width the descriptions line up past.
usize spelled(const PkgCommand &c)
{
    return c.name.size() + (c.args.empty() ? 0 : 1 + c.args.size());
}

// The block, built from the table so a row added later cannot drift. A row at
// a time rather than one buffer: the whole of it is past what a coroutine
// frame may hold, and a heap block would put an allocation on the path that
// reports a mistake.
Task<Result<void>> usage(u32 fd)
{
    usize w = 0;
    for (const PkgCommand &c : TABLE)
        w = max(w, spelled(c));

    Result<void> r = Err(Error::NoMemory);
    if (Task<Result<void>> t = write_all(fd, HEAD))
        r = co_await t;
    for (const PkgCommand &c : TABLE) {
        if (r.is_err())
            co_return r;
        Buf<96> b;
        b.put("    ").put(c.name);
        if (!c.args.empty())
            b.put(' ').put(c.args);
        for (usize i = spelled(c); i < w + GAP; i++)
            b.put(' ');
        b.put(c.help).put('\n');
        if (Task<Result<void>> t = write_all(fd, b.str()))
            r = co_await t;
    }
    co_return r;
}

Task<Result<void>> complain(Str before, Str name, Str after)
{
    Buf<96> b;
    b.put("pkg: ").put(before).put(name).put(after).put('\n');
    co_return co_await write_all(SYS_STDERR, b.str());
}

} // namespace

bool pkg_verbose()
{
    return verbose_on;
}

void pkg_set_verbose(bool on)
{
    verbose_on = on;
}

Task<void> pkg_net_hint(IndexStep step, Error why)
{
    if (step != IndexStep::Fetch && step != IndexStep::Package)
        co_return;
    Str line;
    if (why == Error::Perm)
        line = CROSS_ORIGIN;
    else if (why == Error::Io)
        line = NO_ANSWER;
    else
        co_return;
    if (Task<Result<void>> t = write_all(SYS_STDERR, line))
        co_await t;
}

Task<i32> pkg_load_index(CheckedIndex &c)
{
    Result<String> text = Err(Error::NoMemory);
    if (Task<Result<String>> t = store_slurp(PKG_INDEX))
        text = co_await t;
    if (text.is_err()) {
        if (text.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("pkg", PKG_INDEX, text.error()))
            co_await e;
        co_return 1;
    }
    if (text.value().empty()) {
        if (Task<Result<void>> t = write_all(SYS_STDERR, NO_INDEX))
            co_await t;
        co_return 1;
    }

    Result<void> r = index_read(move(text.value()), c);
    if (r.is_err()) {
        if (Task<void> e = errln("pkg", PKG_INDEX, r.error()))
            co_await e;
        co_return 1;
    }
    co_return 0;
}

Task<Result<void>> pkg_generation(String &path, String &text)
{
    Result<u32> gen = Err(Error::NoMemory);
    if (Task<Result<u32>> t = store_active())
        gen = co_await t;
    if (gen.is_err())
        co_return Err(gen.error());
    if (gen.value() == 0)
        co_return Result<void>();

    if (!pkg_gen_dir(gen.value(), "packages", path))
        co_return Err(Error::NoMemory);
    Result<String> got = Err(Error::NoMemory);
    if (Task<Result<String>> t = store_slurp(path.str()))
        got = co_await t;
    if (got.is_err())
        co_return Err(got.error());
    text = move(got.value());
    co_return Result<void>();
}

Task<i32> pkg_help(Args args)
{
    if (args.size() != 1) {
        if (Task<Result<void>> t = write_all(SYS_STDERR, USAGE_HELP))
            co_await t;
        co_return 2;
    }
    Result<void> r = Err(Error::NoMemory);
    if (Task<Result<void>> t = usage(SYS_STDOUT))
        r = co_await t;
    co_return r.is_ok() ? 0 : 1;
}

Task<i32> pkg_run(Args args)
{
    // The flags are pkg's own, wherever they were typed, so they come out of
    // the words before the table is searched and what is left is the command's
    // argv. Err(Invalid) names the offender in `bad`.
    Vec<Str> words;
    Str bad;
    Result<void> f = take_flags(args.tail(), words, bad);
    if (f.is_err() && f.error() == Error::Invalid) {
        if (Task<Result<void>> t = complain("unknown option: ", bad, ""))
            co_await t;
        if (Task<Result<void>> t = usage(SYS_STDERR))
            co_await t;
        co_return 2;
    }
    if (f.is_err()) {
        if (Task<Result<void>> t = write_all(SYS_STDERR, NO_MEMORY))
            co_await t;
        co_return 1;
    }

    // No command is asking rather than getting it wrong: the block on stdout,
    // and 0, exactly as `pkg help` prints it.
    Args rest{ Span<const Str>(words.data(), words.size()) };
    if (rest.size() == 0) {
        Result<void> r = Err(Error::NoMemory);
        if (Task<Result<void>> t = usage(SYS_STDOUT))
            r = co_await t;
        co_return r.is_ok() ? 0 : 1;
    }

    // The other spelling of asking.
    Str word = rest[0];
    if (word == "-h" || word == "--help")
        word = "help";

    const PkgCommand *c = find(word);
    if (!c) {
        if (Task<Result<void>> t = complain("unknown command: ", word, ""))
            co_await t;
        if (Task<Result<void>> t = usage(SYS_STDERR))
            co_await t;
        co_return 2;
    }
    if (!c->run) {
        if (Task<Result<void>> t = complain("", word, " is not built yet"))
            co_await t;
        co_return 1;
    }

    Task<i32> t = c->run(rest);
    co_return t ? co_await t : 1;
}

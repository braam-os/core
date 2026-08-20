#include "pkg.h"

#include "kernel/fmt.h"
#include "proc/io.h"

namespace {

// Sorted, and explicit: --gc-sections never extracts an unreferenced archive
// member. What each command is for is /share/help's, not a string here.
constexpr PkgCommand TABLE[] = {
    { "autoremove", nullptr }, { "clean", nullptr },   { "files", nullptr },  { "info", nullptr },
    { "install", nullptr },    { "list", nullptr },    { "remove", nullptr }, { "search", nullptr },
    { "update", pkg_update },  { "upgrade", nullptr }, { "verify", nullptr },
};

constexpr Str USAGE  = "usage: pkg <command> [<arg>...]\n";
constexpr Str LEAD   = "commands:";
constexpr Str INDENT = "         ";
constexpr usize WRAP = 78;

const PkgCommand *find(Str name)
{
    for (const PkgCommand &c : TABLE)
        if (c.name == name)
            return &c;
    return nullptr;
}

// The command line, built from the table so a row added later cannot drift.
Task<Result<void>> usage()
{
    Buf<256> b;
    b.put(USAGE).put(LEAD);
    usize col = LEAD.size();
    for (const PkgCommand &c : TABLE) {
        if (col + 1 + c.name.size() > WRAP) {
            b.put('\n').put(INDENT);
            col = INDENT.size();
        }
        b.put(' ').put(c.name);
        col += 1 + c.name.size();
    }
    b.put('\n');
    co_return co_await write_all(SYS_STDERR, b.str());
}

Task<Result<void>> complain(Str before, Str name, Str after)
{
    Buf<96> b;
    b.put("pkg: ").put(before).put(name).put(after).put('\n');
    co_return co_await write_all(SYS_STDERR, b.str());
}

} // namespace

Task<i32> pkg_run(Args args)
{
    Args rest = args.tail();
    if (rest.size() == 0) {
        if (Task<Result<void>> t = usage())
            co_await t;
        co_return 2;
    }

    Str word            = rest[0];
    const PkgCommand *c = find(word);
    if (!c) {
        if (Task<Result<void>> t = complain("unknown command: ", word, ""))
            co_await t;
        if (Task<Result<void>> t = usage())
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

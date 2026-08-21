#include "pkg.h"

#include "db.h"
#include "kernel/fmt.h"
#include "kernel/traits.h"
#include "proc/io.h"
#include "store.h"

namespace {

constexpr Str NO_INDEX = "pkg: no index; run pkg update\n";

// Sorted, and explicit: --gc-sections never extracts an unreferenced archive
// member. What each command is for is /share/help's, not a string here.
constexpr PkgCommand TABLE[] = {
    { "autoremove", pkg_autoremove }, { "clean", nullptr },
    { "files", pkg_files },           { "info", pkg_info },
    { "install", pkg_install },       { "list", pkg_list },
    { "remove", pkg_remove },         { "search", pkg_search },
    { "update", pkg_update },         { "upgrade", pkg_upgrade },
    { "verify", pkg_verify },
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

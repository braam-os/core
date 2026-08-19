#include "cmd/sh/var.h"
#include "decl.h"
#include "kernel/fmt.h"
#include "kernel/string.h"
#include "kernel/vec.h"

namespace {

// Where the usage lines for /bin live. There is no registry to read them out
// of — a program is a file, and a usage line inside a wasm module would mean a
// section for it — so they ship beside the binaries, one `name  usage` line
// each, and a name the file does not mention still lists.
constexpr Str MANIFEST = "/share/help";

// The usage the manifest gives `name`, or an empty Str. A line is the name,
// then whitespace, then the rest of it.
Str usage_of(Str manifest, Str name)
{
    usize at = 0;
    while (at < manifest.size()) {
        usize end = manifest.find('\n', at);
        if (end == Str::npos)
            end = manifest.size();
        Str line = manifest.substr(at, end - at);
        at       = end + 1;

        usize sp = line.find(' ');
        if (sp == Str::npos || line.substr(0, sp) != name)
            continue;
        while (sp < line.size() && line[sp] == ' ')
            sp++;
        return line.substr(sp);
    }
    return Str();
}

bool put(String &out, Str name, Str usage, usize width)
{
    Buf<128> b;
    b.put("  ").put(name);
    for (usize k = name.size(); k < width; k++)
        b.put(' ');
    b.put("  ").put(usage).put('\n');
    return out.append(b.str());
}

} // namespace

// The builtins, then what PATH names. What a program costs to run is not
// something the listing says: every one of them is a binary and runs the same
// way.
Task<i32> builtin_help(Args args, ShIo io)
{
    if (args.size() != 1) {
        co_await write_all(io.err, "usage: help\n");
        co_return 2;
    }

    // Each directory in turn, first occurrence of a name winning: the order the
    // kernel will search them in. A directory that is not there is not an error.
    Str value;
    Str rest = var_get("PATH", value) ? value : SYS_PATH_DEFAULT, dir;
    Vec<String> names;
    while (env_path_next(rest, dir)) {
        Result<Vec<DirEntry>> got = Err(Error::NotFound);
        if (Task<Result<Vec<DirEntry>>> t = list_dir(dir))
            got = co_await t;
        if (got.is_err())
            continue;
        for (DirEntry &e : got.value()) {
            // A program under a second name is SYS_KIND_LINK; a directory is
            // not a program. `echo` and `test` are both a builtin and a file,
            // and the builtin is what typing the name runs.
            if (e.kind == SYS_KIND_DIR || builtin_find(e.name.str()))
                continue;
            bool seen = false;
            for (const String &n : names)
                seen = seen || n.str() == e.name.str();
            if (!seen && !names.push(move(e.name)))
                co_return 1;
        }
    }

    usize width = 0;
    for (const Builtin &b : builtins())
        if (b.name.size() > width)
            width = b.name.size();
    for (const String &n : names)
        if (n.size() > width)
            width = n.size();

    // Built whole and written once, which is the discipline a builtin in a
    // pipeline needs (builtin.h) — `help | grep ls` is exactly that.
    String out;
    for (const Builtin &b : builtins())
        if (!put(out, b.name, b.usage, width))
            co_return 1;

    String manifest;
    if (Task<Result<String>> t = read_file(MANIFEST))
        if (Result<String> r = co_await t; r.is_ok())
            manifest = move(r.value());

    for (const String &n : names) {
        Str usage = usage_of(manifest.str(), n.str());
        if (usage.empty())
            usage = "a program on PATH";
        if (!put(out, n.str(), usage, width))
            co_return 1;
    }

    if (Task<Result<void>> t = write_all(io.out, out.str()))
        co_return (co_await t).is_ok() ? 0 : 1;
    co_return 1;
}

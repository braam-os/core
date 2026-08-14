#include "decl.h"
#include "fs/vfs.h"
#include "kernel/fmt.h"
#include "user/builtin.h"
#include "user/io.h"

namespace {

// Where the usage lines for /bin live. The kernel no longer holds one per
// program — a program is a file now, and reading a usage line out of a wasm
// module would mean a section for it — so they ship beside the binaries, one
// `name  usage` line each, and a name the file does not mention still lists.
constexpr Str MANIFEST = "/share/help";

Task<Result<String>> read_manifest()
{
    FileIo f;
    Task<Result<void>> o = file_open_read(MANIFEST, f);
    if (!o)
        co_return Err(Error::NoMemory);
    if (Result<void> r = co_await o; r.is_err())
        co_return Err(r.error());

    String text;
    for (;;) {
        u8 block[FS_BLOCK];
        Result<usize> r = vfs_read(f.fd, f.off, block, sizeof(block));
        if (r.is_err())
            co_return Err(r.error());
        if (!r.value())
            break;
        f.off += r.value();
        if (!text.append(Str(reinterpret_cast<const char *>(block), r.value())))
            co_return Err(Error::NoMemory);
    }
    co_return move(text);
}

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

Task<Result<void>> put(Stream out, Str name, Str usage, usize width)
{
    Buf<128> b;
    b.put("  ").put(name);
    for (usize k = name.size(); k < width; k++)
        b.put(' ');
    b.put("  ").put(usage).put('\n');
    co_return co_await write_all(out, b.str());
}

} // namespace

// The builtins, then /bin. What tier a name in /bin runs at is not something
// the listing says: that is the point of the tier being the binary's business.
Task<i32> builtin_help(Args args, Stdio io)
{
    if (args.size() != 1) {
        co_await io.err.write("usage: help\n");
        co_return 2;
    }

    Result<Vec<Entry>> bin = Err(Error::NotFound);
    if (Task<Result<Vec<Entry>>> t = vfs_list("/bin"))
        bin = co_await t;

    usize width = 0;
    for (const Builtin &b : builtins())
        if (b.name.size() > width)
            width = b.name.size();
    if (bin.is_ok())
        for (const Entry &e : bin.value())
            if (e.kind == NodeKind::File && e.name.size() > width)
                width = e.name.size();

    for (const Builtin &b : builtins()) {
        Task<Result<void>> t = put(io.out, b.name, b.usage, width);
        if (!t || (co_await t).is_err())
            co_return 1;
    }
    if (bin.is_err())
        co_return 0;

    String manifest;
    if (Task<Result<String>> t = read_manifest())
        if (Result<String> r = co_await t; r.is_ok())
            manifest = move(r.value());

    for (const Entry &e : bin.value()) {
        if (e.kind != NodeKind::File)
            continue;
        Str usage = usage_of(manifest.str(), e.name.str());
        if (usage.empty())
            usage = "a program in /bin";
        Task<Result<void>> t = put(io.out, e.name.str(), usage, width);
        if (!t || (co_await t).is_err())
            co_return 1;
    }
    co_return 0;
}

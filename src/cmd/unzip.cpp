#include "cmd/pkg/unzip.h"

#include "kernel/fmt.h"
#include "proc/io.h"
#include "proc/opt.h"

// /bin/pkg's zip reader over an archive that need not be a package
// (Package_Formats.md §5.2). §5.2 refuses an absolute, backslashed,
// drive-lettered or climbing name, so there is no path check here.

namespace {

constexpr Str USAGE = "usage: unzip [-l] [-p] [-d <dir>] <archive> [<name>...]\n";

// Nothing named takes every entry.
bool wanted(Args names, Str entry)
{
    if (names.size() == 0)
        return true;
    for (usize i = 0; i < names.size(); i++)
        if (names[i] == entry)
            return true;
    return false;
}

usize width_of(u64 n)
{
    usize w = 1;
    for (; n >= 10; n /= 10)
        w++;
    return w;
}

bool join(Str dir, Str name, String &out)
{
    if (dir.empty())
        return out.assign(name);
    return out.assign(dir) && out.push('/') && out.append(name);
}

// Everything before the last `/`, which the write needs to exist.
Str parent_of(Str path)
{
    for (usize i = path.size(); i > 0; i--)
        if (path[i - 1] == '/')
            return path.substr(0, i - 1);
    return Str();
}

// store_write again: that one is braam_pkg's, and linking it here would bring
// the whole package manager.
Task<Result<void>> put_file(Str path, Str bytes)
{
    Result<i32> fd = Err(Error::NoMemory);
    if (Task<Result<i32>> t = open_at(path, SYS_O_WRITE | SYS_O_CREATE | SYS_O_TRUNC))
        fd = co_await t;
    if (fd.is_err())
        co_return Err(fd.error());

    Result<void> w = Err(Error::NoMemory);
    if (Task<Result<void>> t = write_all(u32(fd.value()), bytes))
        w = co_await t;
    if (Task<void> c = close_fd(u32(fd.value())))
        co_await c;
    co_return w;
}

// The sizes right-aligned, then the names. Built whole; a listing is small.
Task<i32> list_them(Span<const ZipEntry> entries, Args names)
{
    u64 big = 0;
    for (const ZipEntry &e : entries)
        if (wanted(names, e.name) && e.size > big)
            big = e.size;

    usize w = width_of(big);
    String out;
    for (const ZipEntry &e : entries) {
        if (!wanted(names, e.name))
            continue;
        Buf<24> n;
        n.put(e.size);
        for (usize i = n.str().size(); i < w; i++)
            if (!out.push(' '))
                co_return 1;
        if (!out.append(n.str()) || !out.append("  ") || !out.append(e.name) || !out.push('\n'))
            co_return 1;
    }

    if (out.empty())
        co_return 0;
    if (Result<void> x = co_await write_all(SYS_STDOUT, out.str()); x.is_err())
        co_return 1;
    co_return 0;
}

Error why_of(ZipRead r)
{
    if (r == ZipRead::Unsupported)
        return Error::Unsupported;
    if (r == ZipRead::NoMemory)
        return Error::NoMemory;
    return Error::Invalid;
}

} // namespace

Task<i32> proc_main(Args args)
{
    bool list = false, pipe = false;
    Str dest;

    OptParse p(args, Opts{ "lp", "d" });
    for (Opt o;;) {
        Result<bool> r = p.next(o);
        if (r.is_err()) {
            Buf<64> b;
            b.put("unzip: ")
                .put(r.error() == Error::NotFound ? "option requires an argument -- "
                                                  : "illegal option -- ")
                .put(o.name)
                .put('\n');
            co_await write_all(SYS_STDERR, b.str());
            co_await write_all(SYS_STDERR, USAGE);
            co_return 2;
        }
        if (!r.value())
            break;
        if (o.name == 'l')
            list = true;
        else if (o.name == 'p')
            pipe = true;
        else
            dest = o.value;
    }

    // -l says what is in there and -p writes it out; asking for both says
    // nothing about which stdout is for.
    Args rest = p.rest();
    if (rest.size() == 0 || (list && pipe)) {
        co_await write_all(SYS_STDERR, USAGE);
        co_return 2;
    }

    Str archive = rest[0];
    Args names{ rest.v.subspan(1) };

    Result<String> got = Err(Error::NoMemory);
    if (Task<Result<String>> t = read_file(archive))
        got = co_await t;
    if (got.is_err()) {
        if (got.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("unzip", archive, got.error()))
            co_await e;
        co_return 1;
    }

    Vec<ZipEntry> entries;
    if (ZipRead how = zip_entries(got.value().str(), entries); how != ZipRead::Ok) {
        if (Task<void> e = errln("unzip", archive, why_of(how)))
            co_await e;
        co_return 1;
    }

    // A name no entry carries is reported; the rest still run.
    i32 status = 0;
    for (usize i = 0; i < names.size(); i++) {
        bool found = false;
        for (const ZipEntry &e : entries)
            found = found || e.name == names[i];
        if (found)
            continue;
        if (Task<void> e = errln("unzip", names[i], Error::NotFound))
            co_await e;
        status = 1;
    }

    if (list) {
        i32 bad = 1;
        if (Task<i32> t = list_them(entries, names))
            bad = co_await t;
        co_return bad != 0 ? bad : status;
    }

    // One entry inflated at a time, written and freed — unpack()'s discipline.
    for (const ZipEntry &e : entries) {
        if (!wanted(names, e.name))
            continue;

        Result<String> bytes = Err(Error::NoMemory);
        if (Task<Result<String>> t = zip_read(e))
            bytes = co_await t;
        if (bytes.is_err()) {
            if (bytes.error() == Error::Cancelled)
                co_return 130;
            if (Task<void> x = errln("unzip", e.name, bytes.error()))
                co_await x;
            status = 1;
            continue;
        }

        if (pipe) {
            if (Result<void> w = co_await write_all(SYS_STDOUT, bytes.value().str()); w.is_err())
                co_return 1;
            continue;
        }

        String path;
        if (!join(dest, e.name, path))
            co_return 1;

        Result<void> done = Err(Error::NoMemory);
        Str parent        = parent_of(path.str());
        if (parent.empty()) {
            done = Result<void>();
        } else if (Task<Result<void>> t = make_dir_all(parent)) {
            done = co_await t;
        }
        if (done.is_ok())
            if (Task<Result<void>> t = put_file(path.str(), bytes.value().str()))
                done = co_await t;

        if (done.is_err()) {
            if (done.error() == Error::Cancelled)
                co_return 130;
            if (Task<void> x = errln("unzip", path.str(), done.error()))
                co_await x;
            status = 1;
        }
    }

    co_return status;
}

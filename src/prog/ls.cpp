#include "fs/vfs.h"
#include "kernel/fmt.h"
#include "user/io.h"
#include "user/prog.h"

namespace {

// A directory's entries, one per line; -l adds the kind and the size. The
// trailing slash is not optional dressing: it is the only thing distinguishing
// a directory from a file in the short form.
Task<i32> show_dir(Str path, Stdio io, bool detail)
{
    Task<Result<Vec<Entry>>> t = vfs_list(path);
    if (!t)
        co_return 1;

    Result<Vec<Entry>> r = co_await t;
    if (r.is_err()) {
        co_await io.err.write("ls: ");
        co_await io.err.write(path);
        co_await io.err.write(": ");
        co_await io.err.write(error_name(r.error()));
        co_await io.err.write("\n");
        co_return 1;
    }

    for (const Entry &e : r.value()) {
        Buf<128> b;
        if (detail)
            b.put(e.kind == NodeKind::Dir ? "dir  " : "file ").put(e.size).put(' ');
        b.put(e.name.str());
        if (e.kind == NodeKind::Dir)
            b.put('/');
        b.put('\n');
        if ((co_await write_all(io.out, b.str())).is_err())
            co_return 1;
    }
    co_return 0;
}

} // namespace

// The registry that M4's `ls` listed is now mounted on /bin, so this is an
// ordinary directory walk and `ls /bin` still shows the programs.
BRAAM_PROGRAM(prog_ls, "ls", "[-l] [<path>...] — list directories")
{
    bool detail = false;
    usize first = 1;
    for (; first < args.size(); first++) {
        if (args[first] == "-l")
            detail = true;
        else
            break;
    }

    usize count = args.size() - first;
    i32 status  = 0;

    for (usize k = 0; k == 0 || k < count; k++) {
        Str path = count ? args[first + k] : Str(".");

        Task<Result<Stat>> st = vfs_stat(path);
        if (!st)
            co_return 1;
        Result<Stat> s = co_await st;
        if (s.is_err()) {
            status = 1;
            co_await io.err.write("ls: ");
            co_await io.err.write(path);
            co_await io.err.write(": ");
            co_await io.err.write(error_name(s.error()));
            co_await io.err.write("\n");
            continue;
        }

        // A named file lists as itself, which is what makes `ls -l notes`
        // report a size rather than an error.
        if (s.value().kind != NodeKind::Dir) {
            Buf<128> b;
            if (detail)
                b.put("file ").put(s.value().size).put(' ');
            b.put(path).put('\n');
            if ((co_await write_all(io.out, b.str())).is_err())
                co_return 1;
            continue;
        }

        if (count > 1) {
            Buf<96> b;
            b.put(path).put(":\n");
            if ((co_await write_all(io.out, b.str())).is_err())
                co_return 1;
        }

        Task<i32> t = show_dir(path, io, detail);
        if (!t)
            co_return 1;
        if (co_await t)
            status = 1;
        if (count > 1 && k + 1 < count && (co_await write_all(io.out, "\n")).is_err())
            co_return 1;
    }

    co_return status;
}

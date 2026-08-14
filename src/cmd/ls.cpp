#include "kernel/fmt.h"
#include "proc/io.h"

namespace {

// A directory's entries, one per line; -l adds the kind and the size. The
// trailing slash is not optional dressing: it is the only thing distinguishing
// a directory from a file in the short form.
Task<i32> show_dir(Str path, bool detail)
{
    Result<Vec<DirEntry>> r = Err(Error::NoMemory);
    if (Task<Result<Vec<DirEntry>>> t = list_dir(path))
        r = co_await t;
    if (r.is_err()) {
        if (r.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("ls", path, r.error()))
            co_await e;
        co_return 1;
    }

    for (const DirEntry &e : r.value()) {
        Buf<128> b;
        if (detail)
            b.put(e.kind == SYS_KIND_DIR ? Str("dir  ") : Str("file ")).put(e.size).put(' ');
        b.put(e.name.str());
        if (e.kind == SYS_KIND_DIR)
            b.put('/');
        b.put('\n');
        if ((co_await write_all(SYS_STDOUT, b.str())).is_err())
            co_return 1;
    }
    co_return 0;
}

} // namespace

Task<i32> proc_main(Args args)
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

        Result<FileInfo> s = Err(Error::NoMemory);
        if (Task<Result<FileInfo>> t = stat_of(path))
            s = co_await t;
        if (s.is_err()) {
            if (s.error() == Error::Cancelled)
                co_return 130;
            status = 1;
            if (Task<void> e = errln("ls", path, s.error()))
                co_await e;
            continue;
        }

        // A named file lists as itself, which is what makes `ls -l notes`
        // report a size rather than an error.
        if (s.value().kind != SYS_KIND_DIR) {
            Buf<128> b;
            if (detail)
                b.put("file ").put(s.value().size).put(' ');
            b.put(path).put('\n');
            if ((co_await write_all(SYS_STDOUT, b.str())).is_err())
                co_return 1;
            continue;
        }

        if (count > 1) {
            Buf<96> b;
            b.put(path).put(":\n");
            if ((co_await write_all(SYS_STDOUT, b.str())).is_err())
                co_return 1;
        }

        Task<i32> t = show_dir(path, detail);
        if (!t)
            co_return 1;
        if (i32 bad = co_await t) {
            if (bad == 130)
                co_return 130;
            status = 1;
        }
        if (count > 1 && k + 1 < count && (co_await write_all(SYS_STDOUT, "\n")).is_err())
            co_return 1;
    }

    co_return status;
}

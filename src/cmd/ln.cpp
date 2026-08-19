#include "fs/path.h"
#include "proc/io.h"
#include "proc/opt.h"

namespace {

// One link, replacing what stands there when `force`.
Task<Result<void>> link_one(Str target, Str name, bool force)
{
    if (force) {
        // Removed without being followed; nothing to remove is not an error.
        if (Task<Result<void>> t = remove_path(name, false))
            (void)co_await t;
    }
    if (Task<Result<void>> t = make_link(target, name))
        co_return co_await t;
    co_return Err(Error::NoMemory);
}

} // namespace

Task<i32> proc_main(Args args)
{
    bool sym = false, force = false;
    OptParse p(args, Opts{ "sf", "" });
    for (Opt o;;) {
        Result<bool> r = p.next(o);
        if (r.is_err()) {
            co_await write_all(SYS_STDERR, "ln: bad option\n");
            co_return 2;
        }
        if (!r.value())
            break;
        if (o.name == 's')
            sym = true;
        else
            force = true;
    }

    Args rest = p.rest();
    if (rest.size() < 2) {
        co_await write_all(SYS_STDERR, "usage: ln -s <target> <name>\n");
        co_return 2;
    }

    // No hard links: OPFS keeps no link count (Concept.md §5.2).
    if (!sym) {
        co_await write_all(SYS_STDERR, "ln: only symbolic links; use -s\n");
        co_return 2;
    }

    // With one target the last operand is the name; with more it is the
    // directory they go in.
    bool into_dir = rest.size() > 2;
    Str last      = rest[rest.size() - 1];
    if (!into_dir) {
        Result<FileInfo> s = Err(Error::NoMemory);
        if (Task<Result<FileInfo>> t = stat_of(last, false))
            s = co_await t;
        if (s.is_err() && s.error() == Error::Cancelled)
            co_return 130;
        into_dir = s.is_ok() && s.value().kind == SYS_KIND_DIR;
    }

    i32 status = 0;
    for (usize i = 0; i + 1 < rest.size(); i++) {
        Str target = rest[i];

        String name;
        if (into_dir) {
            if (path_join(last, path_basename(target), name).is_err())
                co_return 1;
        } else if (!name.assign(last)) {
            co_return 1;
        }

        Result<void> r = Err(Error::NoMemory);
        if (Task<Result<void>> t = link_one(target, name.str(), force))
            r = co_await t;
        if (r.is_ok())
            continue;
        if (r.error() == Error::Cancelled)
            co_return 130;

        status = 1;
        if (Task<void> e = errln("ln", name.str(), r.error()))
            co_await e;
    }
    co_return status;
}

#include "fs/path.h"
#include "proc/io.h"

// The universally available way in (Concept.md §5.4): a file picker, and the
// bytes land in /import. The picker needs the page's transient activation,
// which the keystroke that ran this command still holds.
Task<i32> proc_main(Args args)
{
    Str dest = args.size() > 1 ? args[1] : Str("/import");
    if (args.size() > 2) {
        co_await write_all(SYS_STDERR, "usage: import [<dir>]\n");
        co_return 2;
    }

    Result<Chosen> got = Err(Error::NoMemory);
    if (Task<Result<Chosen>> t = pick())
        got = co_await t;
    if (got.is_err()) {
        if (got.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("import", Str(), got.error()))
            co_await e;
        co_return 1;
    }

    const Chosen &c = got.value();
    i32 status      = 0;
    for (usize i = 0; i < c.names.size(); i++) {
        String path;
        if (path_join(dest, c.names[i].str(), path).is_err())
            co_return 1;

        Result<i32> in = Err(Error::NoMemory);
        if (Task<Result<i32>> t = pick_open(c, i))
            in = co_await t;
        if (in.is_err())
            co_return in.error() == Error::Cancelled ? 130 : 1;

        Result<i32> out = Err(Error::NoMemory);
        if (Task<Result<i32>> t = open_at(path.str(), SYS_O_WRITE | SYS_O_CREATE | SYS_O_TRUNC))
            out = co_await t;
        if (out.is_err()) {
            if (out.error() == Error::Cancelled)
                co_return 130;
            status = 1;
            if (Task<void> e = errln("import", path.str(), out.error()))
                co_await e;
            if (Task<void> k = close_fd(u32(in.value())))
                co_await k;
            continue;
        }

        for (;;) {
            Result<String> chunk = Err(Error::NoMemory);
            if (Task<Result<String>> t = read_chunk(u32(in.value())))
                chunk = co_await t;
            if (chunk.is_err()) {
                if (chunk.error() == Error::Closed)
                    break;
                co_return chunk.error() == Error::Cancelled ? 130 : 1;
            }
            if ((co_await write_all(u32(out.value()), chunk.value().str())).is_err()) {
                status = 1;
                break;
            }
        }

        if (Task<void> k = close_fd(u32(in.value())))
            co_await k;
        if (Task<void> k = close_fd(u32(out.value())))
            co_await k;

        if ((co_await write_all(SYS_STDOUT, path.str())).is_err())
            co_return 1;
        if ((co_await write_all(SYS_STDOUT, "\n")).is_err())
            co_return 1;
    }

    co_return status;
}

#include "fs/path.h"
#include "fs/vfs.h"
#include "svc/xfer.h"
#include "user/io.h"
#include "user/prog.h"

// The universally available way in (Concept.md §5.4): a file picker, and the
// bytes land in /mnt/import. The picker needs the page's transient activation,
// which the keystroke that ran this command still holds.
BRAAM_PROGRAM(prog_import, "import", "[<dir>] — copy files from the browser into /mnt/import")
{
    Str dest = args.size() > 1 ? args[1] : Str("/mnt/import");
    if (args.size() > 2) {
        co_await io.err.write("usage: import [<dir>]\n");
        co_return 2;
    }

    Result<Picked> got = Err(Error::NoMemory);
    if (Task<Result<Picked>> t = pick_files())
        got = co_await t;
    if (got.is_err()) {
        if (got.error() == Error::Cancelled)
            co_return 130;
        co_await io.err.write("import: ");
        co_await io.err.write(error_name(got.error()));
        co_await io.err.write("\n");
        co_return 1;
    }

    const Picked &p = got.value();
    i32 status      = 0;
    for (usize i = 0; i < p.count; i++) {
        Result<String> name = Err(Error::NoMemory);
        if (Task<Result<String>> t = pick_name(p, i))
            name = co_await t;
        if (name.is_err())
            co_return name.error() == Error::Cancelled ? 130 : 1;

        String path;
        if (path_join(dest, name.value().str(), path).is_err())
            co_return 1;

        Result<i32> fd = Err(Error::NoMemory);
        if (Task<Result<i32>> t = vfs_open(path.str(), O_WRITE | O_CREATE | O_TRUNC))
            fd = co_await t;
        if (fd.is_err()) {
            co_await io.err.write("import: ");
            co_await io.err.write(path.str());
            co_await io.err.write(": ");
            co_await io.err.write(error_name(fd.error()));
            co_await io.err.write("\n");
            status = 1;
            continue;
        }

        FileIo out(fd.value());
        for (;;) {
            Result<String> chunk = Err(Error::NoMemory);
            if (Task<Result<String>> t = pick_read(p, i, out.off))
                chunk = co_await t;
            if (chunk.is_err())
                co_return chunk.error() == Error::Cancelled ? 130 : 1;
            if (chunk.value().empty())
                break;

            Result<usize> put =
                vfs_write(out.fd, out.off, reinterpret_cast<const u8 *>(chunk.value().data()),
                          chunk.value().size());
            if (put.is_err()) {
                co_await io.err.write("import: ");
                co_await io.err.write(path.str());
                co_await io.err.write(": ");
                co_await io.err.write(error_name(put.error()));
                co_await io.err.write("\n");
                status = 1;
                break;
            }
            out.off += put.value();
        }

        if ((co_await write_all(io.out, path.str())).is_err())
            co_return 1;
        if ((co_await write_all(io.out, "\n")).is_err())
            co_return 1;
    }

    co_return status;
}

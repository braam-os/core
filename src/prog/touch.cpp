#include "fs/vfs.h"
#include "user/io.h"
#include "user/prog.h"

// There are no timestamps yet, so this creates and nothing else. It exists
// because `> file` is the only other way to make an empty file, and that reads
// like a mistake.
BRAAM_PROGRAM(prog_touch, "touch", "<file>... — create empty files")
{
    if (args.size() < 2) {
        co_await io.err.write("usage: touch <file>...\n");
        co_return 2;
    }

    i32 status = 0;
    for (usize i = 1; i < args.size(); i++) {
        Result<i32> r = Err(Error::NoMemory);
        if (Task<Result<i32>> t = vfs_open(args[i], O_WRITE | O_CREATE))
            r = co_await t;
        if (r.is_ok()) {
            vfs_close(r.value());
            continue;
        }

        status = 1;
        co_await io.err.write("touch: ");
        co_await io.err.write(args[i]);
        co_await io.err.write(": ");
        co_await io.err.write(error_name(r.error()));
        co_await io.err.write("\n");
    }
    co_return status;
}

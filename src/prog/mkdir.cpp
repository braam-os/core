#include "fs/vfs.h"
#include "user/io.h"
#include "user/prog.h"

BRAAM_PROGRAM(prog_mkdir, "mkdir", "<dir>... — create directories")
{
    if (args.size() < 2) {
        co_await io.err.write("usage: mkdir <dir>...\n");
        co_return 2;
    }

    i32 status = 0;
    for (usize i = 1; i < args.size(); i++) {
        Result<void> r = Err(Error::NoMemory);
        if (Task<Result<void>> t = vfs_mkdir(args[i]))
            r = co_await t;
        if (r.is_ok())
            continue;

        status = 1;
        co_await io.err.write("mkdir: ");
        co_await io.err.write(args[i]);
        co_await io.err.write(": ");
        co_await io.err.write(error_name(r.error()));
        co_await io.err.write("\n");
    }
    co_return status;
}

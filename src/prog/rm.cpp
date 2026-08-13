#include "fs/vfs.h"
#include "user/io.h"
#include "user/prog.h"

BRAAM_PROGRAM(prog_rm, "rm", "[-r] <path>... — remove files, or directories with -r")
{
    bool all = false;
    usize i  = 1;
    for (; i < args.size(); i++) {
        if (args[i] == "-r")
            all = true;
        else
            break;
    }
    if (i >= args.size()) {
        co_await io.err.write("usage: rm [-r] <path>...\n");
        co_return 2;
    }

    i32 status = 0;
    for (; i < args.size(); i++) {
        Result<void> r = Err(Error::NoMemory);
        if (Task<Result<void>> t = vfs_remove(args[i], all))
            r = co_await t;
        if (r.is_ok())
            continue;

        status = 1;
        co_await io.err.write("rm: ");
        co_await io.err.write(args[i]);
        co_await io.err.write(": ");
        co_await io.err.write(error_name(r.error()));
        co_await io.err.write("\n");
    }
    co_return status;
}

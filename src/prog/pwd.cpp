#include "fs/vfs.h"
#include "user/io.h"
#include "user/prog.h"

BRAAM_PROGRAM(prog_pwd, "pwd", "print the working directory")
{
    if (args.size() > 1) {
        co_await io.err.write("usage: pwd\n");
        co_return 2;
    }

    if ((co_await write_all(io.out, vfs_cwd())).is_err())
        co_return 1;
    if ((co_await write_all(io.out, "\n")).is_err())
        co_return 1;
    co_return 0;
}

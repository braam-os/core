#include "fs/vfs.h"
#include "user/io.h"
#include "user/prog.h"

// The working directory is one global, not per-process: a program is handed
// (Args, Stdio) and nothing else until M8's ABI gives it a context of its own.
// With one shell running, the two are indistinguishable.
BRAAM_PROGRAM(prog_cd, "cd", "[<dir>] — change the working directory, /home by default")
{
    if (args.size() > 2) {
        co_await io.err.write("usage: cd [<dir>]\n");
        co_return 2;
    }

    Str where            = args.size() == 2 ? args[1] : Str("/home");
    Task<Result<void>> t = vfs_chdir(where);
    if (!t)
        co_return 1;

    Result<void> r = co_await t;
    if (r.is_err()) {
        co_await io.err.write("cd: ");
        co_await io.err.write(where);
        co_await io.err.write(": ");
        co_await io.err.write(error_name(r.error()));
        co_await io.err.write("\n");
        co_return 1;
    }
    co_return 0;
}

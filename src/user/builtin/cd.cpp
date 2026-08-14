#include "decl.h"
#include "fs/vfs.h"
#include "user/io.h"

// A builtin because the working directory is one global rather than per-process
// (Concept.md §5.1): a process is isolated in address space, memory and
// descriptors, and not in the namespace it can name, so a `cd` syscall would
// hand one process the authority to move every other one's feet.
Task<i32> builtin_cd(Args args, Stdio io)
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

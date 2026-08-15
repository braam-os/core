#include "decl.h"

// A builtin because the directory it moves is *this process's own*, and that is
// what a typed command inherits: `Sys::Spawn` hands a child the shell's cwd
// (Concept.md §5.1). A `cd` in /bin would move a child's own and exit, leaving
// the shell exactly where it was.
Task<i32> builtin_cd(Args args, ShIo io)
{
    if (args.size() > 2) {
        co_await write_all(io.err, "usage: cd [<dir>]\n");
        co_return 2;
    }

    Str where              = args.size() == 2 ? args[1] : Str("/home");
    Task<Result<String>> t = cwd_set(where);
    Result<String> r       = t ? co_await t : Err(Error::NoMemory);
    if (r.is_err()) {
        if (Task<void> e = errln("cd", where, r.error()))
            co_await e;
        co_return 1;
    }
    co_return 0;
}

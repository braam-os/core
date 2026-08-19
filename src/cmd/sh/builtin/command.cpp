#include "cmd/sh/condrun.h"
#include "cmd/sh/job.h"
#include "cmd/sh/var.h"
#include "decl.h"
#include "fs/path.h"
#include "kernel/string.h"

namespace {

// Where the shell looks, which is where the kernel will look: its own PATH if
// it has one, and the kernel's default if it has none.
Str search_path()
{
    Str v;
    return var_get("PATH", v) ? v : SYS_PATH_DEFAULT;
}

// The path `word` would run, or an empty Str. A word with a slash is a path and
// is never searched — the same rule exec_resolve applies.
Task<Result<String>> where(Str word)
{
    String path;
    if (word.contains("/")) {
        if (!path.assign(word))
            co_return Err(Error::NoMemory);
        Task<bool> t = file_runnable(path.str());
        if (t && co_await t)
            co_return move(path);
        co_return String();
    }

    Str rest = search_path(), dir;
    while (env_path_next(rest, dir)) {
        CO_TRY_VOID(path_join(dir, word, path));
        Task<bool> t = file_runnable(path.str());
        if (t && co_await t)
            co_return move(path);
    }
    co_return String();
}

} // namespace

// The query half only: `command <cmd>` — run a command with function lookup
// suppressed — would have to reach the stage resolution in job.cpp, and a
// builtin is not where that lives.
Task<i32> builtin_command(Args args, ShIo io)
{
    if (args.size() < 3 || args[1] != "-v") {
        co_await write_all(io.err, "usage: command -v <name>...\n");
        co_return 2;
    }

    String out;
    bool all = true;
    for (usize i = 2; i < args.size(); i++) {
        Str w = args[i];

        // A function, then a builtin, then PATH: what a command word resolves
        // as, reported as the name itself for the two that have no file.
        if (!w.contains("/") && (func_exists(w) || builtin_find(w))) {
            if (!out.append(w) || !out.push('\n'))
                co_return 1;
            continue;
        }

        Task<Result<String>> t = where(w);
        Result<String> r       = t ? co_await t : Err(Error::NoMemory);
        if (r.is_err())
            co_return 1;
        if (r.value().empty()) {
            all = false;
            continue;
        }
        if (!out.append(r.value().str()) || !out.push('\n'))
            co_return 1;
    }

    if (Task<Result<void>> t = write_all(io.out, out.str()))
        if ((co_await t).is_err())
            co_return 1;
    co_return all ? 0 : 1;
}

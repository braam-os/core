#include "script.h"

#include "db.h"
#include "kernel/args.h"
#include "kernel/string.h"
#include "kernel/vec.h"
#include "proc/io.h"

// A child of /bin/pkg sharing its stdio, spawned as `/bin/sh <file>`.

namespace {

constexpr Str SHELL = "/bin/sh";

} // namespace

Task<Result<i32>> script_run(Str name, Str version, ZipMeta kind, Span<const Str> args)
{
    Str leaf = zip_meta_name(kind);
    if (leaf.empty())
        co_return 0;

    String path;
    if (!pkg_store_dir(name, version, leaf, path))
        co_return Err(Error::NoMemory);

    // Most packages carry none, and that must cost a stat and nothing else.
    Result<FileInfo> st = Err(Error::NoMemory);
    if (Task<Result<FileInfo>> t = stat_of(path.str()))
        st = co_await t;
    if (st.is_err())
        co_return st.error() == Error::NotFound ? Result<i32>(0) : Err(st.error());
    if (st.value().kind != SYS_KIND_FILE)
        co_return 0;

    Vec<Str> argv;
    if (!argv.push(SHELL) || !argv.push(path.str()))
        co_return Err(Error::NoMemory);
    for (Str a : args)
        if (!argv.push(a))
            co_return Err(Error::NoMemory);

    Result<u32> pid = Err(Error::NoMemory);
    if (Task<Result<u32>> t = spawn(Args{ argv }))
        pid = co_await t;
    if (pid.is_err())
        co_return Err(pid.error());

    Result<Exited> done = Err(Error::NoMemory);
    if (Task<Result<Exited>> t = wait_child(pid.value()))
        done = co_await t;
    if (done.is_err()) {
        // ^C abandoned the wait; the child is still running.
        if (Task<Result<void>> k = kill_child(pid.value()))
            co_await k;
        co_return Err(done.error());
    }
    co_return done.value().status;
}

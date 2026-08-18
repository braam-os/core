#include "kernel/string.h"
#include "proc/io.h"
#include "proc/rt.h"

// Prints the environment, or runs a command with it changed. The assignments a
// shell's `x=1 prog` prefix does for one stage, spelled out — and the only way
// to reach a child's environment from a program.

namespace {

// This process's environment with `adds` over the top, a later word standing in
// for an earlier one of the same name. `store` owns the added bytes.
bool build(Args adds, bool clean, String &store, Vec<Str> &out)
{
    if (!clean) {
        if (!out.reserve(proc_env_count()))
            return false;
        for (usize i = 0; i < proc_env_count(); i++)
            if (!out.push(proc_env_at(i)))
                return false;
    }

    usize total = 0;
    for (usize i = 0; i < adds.size(); i++)
        total += adds[i].size();
    if (!store.reserve(total))
        return false;
    for (usize i = 0; i < adds.size(); i++)
        if (!store.append(adds[i]))
            return false;

    usize at = 0;
    for (usize i = 0; i < adds.size(); i++) {
        Str word = store.str().substr(at, adds[i].size());
        at += adds[i].size();

        usize nlen = word.find('=') + 1;
        for (usize j = 0; j < out.size(); j++)
            if (out[j].size() >= nlen && out[j].substr(0, nlen) == word.substr(0, nlen)) {
                out.erase(j);
                break;
            }
        if (!out.push(word))
            return false;
    }
    return true;
}

} // namespace

Task<i32> proc_main(Args args)
{
    usize at   = 1;
    bool clean = false;
    if (at < args.size() && args[at] == "-i") {
        clean = true;
        at++;
    }

    // The assignments, which end at the first word that is not one.
    usize first = at;
    while (at < args.size() && args[at].find('=') != Str::npos && args[at].find('=') != 0)
        at++;

    String store;
    Vec<Str> env;
    if (!build(Args{ args.v.subspan(first, at - first) }, clean, store, env)) {
        co_await write_all(SYS_STDERR, "env: out of memory\n");
        co_return 1;
    }

    if (at >= args.size()) {
        // Buffered and written once, the way a builtin does it.
        String out;
        for (usize i = 0; i < env.size(); i++)
            if (!out.append(env[i]) || !out.push('\n')) {
                co_await write_all(SYS_STDERR, "env: out of memory\n");
                co_return 1;
            }
        Result<void> r = Err(Error::NoMemory);
        if (Task<Result<void>> t = write_all(SYS_STDOUT, out.str()))
            r = co_await t;
        co_return r.is_ok() ? 0 : 1;
    }

    Args ev{ Span<const Str>(env.data(), env.size()) };
    Result<u32> pid = Err(Error::NoMemory);
    if (Task<Result<u32>> t = spawn(Args{ args.v.subspan(at) }, ChildIo{}, &ev))
        pid = co_await t;
    if (pid.is_err()) {
        if (pid.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("env", args[at], pid.error()))
            co_await e;
        co_return pid.error() == Error::NotFound ? 127 : 126;
    }

    Result<Exited> done = Err(Error::NoMemory);
    if (Task<Result<Exited>> t = wait_child(pid.value()))
        done = co_await t;
    if (done.is_err())
        co_return done.error() == Error::Cancelled ? 130 : 1;
    co_return done.value().status;
}

#include "kernel/text.h"
#include "proc/io.h"

// Runs a command and kills it if it is still going after a delay. The first
// program that supervises another one, which is what the process family of
// Concept.md §4.3 exists for: a builtin could not do it (it would be running in
// the shell's own job) and nothing else in /bin can start a process.
//
// Milliseconds, for the reason `sleep` gives: no float parser, and the smoke
// test wants an exact number.

namespace {

// The alarm, in a task of its own. It has to be the second task rather than the
// root, because a process ends when its *root* returns (Concept.md §4.3) — so
// the wait belongs there, and the kernel cancels whatever this one still has
// outstanding when it does.
bool g_fired;

Task<i32> alarm(u32 pid, u32 ms)
{
    Result<void> r = Err(Error::NoMemory);
    if (Task<Result<void>> t = sleep_for(ms))
        r = co_await t;
    if (r.is_err())
        co_return 0; // cancelled: the child finished first, which is the point

    g_fired = true;
    if (Task<Result<void>> t = kill_child(pid))
        co_await t;
    co_return 0;
}

} // namespace

Task<i32> proc_main(Args args)
{
    Option<u32> ms = args.size() >= 3 ? parse_u32(args[1]) : None;
    if (!ms.has_value()) {
        co_await write_all(SYS_STDERR, "usage: timeout <ms> <command> [<arg>...]\n");
        co_return 2;
    }

    // Stdio is shared rather than moved, so the child writes where this process
    // writes and `timeout 500 ls | wc` is one pipeline with one reader.
    Result<u32> pid = Err(Error::NoMemory);
    if (Task<Result<u32>> t = spawn(Args{ args.v.subspan(2) }))
        pid = co_await t;
    if (pid.is_err()) {
        if (pid.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("timeout", args[2], pid.error()))
            co_await e;
        co_return pid.error() == Error::NotFound ? 127 : 126;
    }

    if (!proc_spawn(alarm(pid.value(), ms.value()))) {
        co_await write_all(SYS_STDERR, "timeout: no room for a timer\n");
        if (Task<Result<void>> t = kill_child(pid.value()))
            co_await t;
        co_return 1;
    }

    Result<Exited> done = Err(Error::NoMemory);
    if (Task<Result<Exited>> t = wait_child(pid.value()))
        done = co_await t;
    if (done.is_err())
        co_return done.error() == Error::Cancelled ? 130 : 1;

    // 124 is what GNU timeout reports when the delay is what ended it, and it
    // is worth keeping distinct from whatever the command would have said.
    co_return g_fired ? 124 : done.value().status;
}

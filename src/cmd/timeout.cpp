#include "kernel/text.h"
#include "proc/io.h"

// Runs a command and kills it if it is still going after a delay. The first
// program that supervises another one, which is what the process family of
// Concept.md §4.3 exists for: a builtin could not do it (it would be running in
// the shell's own job) and nothing else in /bin can start a process.
//
// The delay is seconds, or milliseconds with -m, as `sleep`'s is.

namespace {

constexpr u32 MAX_SECS = 4294967; // as many as convert to ms inside a u32

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
    usize i    = 1;
    bool milli = i < args.size() && args[i] == "-m";
    if (milli)
        i++;

    Option<u32> n = args.size() >= i + 2 ? parse_u32(args[i]) : None;
    if (!n.has_value() || (!milli && n.value() > MAX_SECS)) {
        co_await write_all(SYS_STDERR, "usage: timeout [-m] <seconds> <command> [<arg>...]\n");
        co_return 2;
    }
    u32 ms = milli ? n.value() : n.value() * 1000;
    i++;

    // Stdio is shared rather than moved, so the child writes where this process
    // writes and `timeout 5 ls | wc` is one pipeline with one reader.
    Result<u32> pid = Err(Error::NoMemory);
    if (Task<Result<u32>> t = spawn(Args{ args.v.subspan(i) }))
        pid = co_await t;
    if (pid.is_err()) {
        if (pid.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("timeout", args[i], pid.error()))
            co_await e;
        co_return pid.error() == Error::NotFound ? 127 : 126;
    }

    if (!proc_spawn(alarm(pid.value(), ms))) {
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

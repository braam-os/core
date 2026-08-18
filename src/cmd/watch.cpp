#include "kernel/text.h"
#include "proc/io.h"

// Runs a command over and over, showing only its latest output. The one caller
// of Sys::Pipe, and the reason the operation exists: a supervisor that wants
// what its child printed rather than wanting it printed.
//
// Two orderings in here are load bearing, and both are the pipe's rather than
// this program's.

namespace {

constexpr u32 DEFAULT_SECS = 2;
constexpr u32 MAX_SECS     = 4294967; // as many as convert to ms inside a u32

// The most of a child's output to hold. A command that prints for ever must not
// grow this process until its 16 MB cap stops it.
constexpr usize MAX_CAPTURE = 32 * 1024;

// One round: the child writes into a pipe, and this reads it to the end.
Task<Result<i32>> once(Args cmd, String &out)
{
    Result<Piped> pipe = Err(Error::NoMemory);
    if (Task<Result<Piped>> t = make_pipe())
        pipe = co_await t;
    if (pipe.is_err())
        co_return Err(pipe.error());

    // The write end is *moved* into the child, so this process has no copy of
    // it left open. That is what closes the channel when the child goes, and
    // therefore what ends the read below — a copy kept here would park for ever
    // on a pipe whose only writer had already exited.
    Result<u32> pid = Err(Error::NoMemory);
    ChildIo io{ SYS_STDIN, u32(pipe.value().w), SYS_STDERR };
    if (Task<Result<u32>> t = spawn(cmd, io))
        pid = co_await t;
    if (pid.is_err()) {
        co_await close_fd(u32(pipe.value().r));
        co_return Err(pid.error());
    }

    // Drained before the wait, not after. A child parked in a full pipe has not
    // exited, so waiting first would be this process waiting on a child waiting
    // on this process — a deadlock the kernel cannot break, only ^C can.
    Result<i32> bad = 0;
    out.clear();
    for (;;) {
        Result<String> r = Err(Error::NoMemory);
        if (Task<Result<String>> t = read_chunk(u32(pipe.value().r)))
            r = co_await t;
        if (r.is_err()) {
            if (r.error() != Error::Closed)
                bad = Err(r.error());
            break;
        }
        if (out.size() < MAX_CAPTURE && !out.append(r.value().str())) {
            bad = Err(Error::NoMemory);
            break;
        }
    }

    if (bad.is_err()) {
        if (Task<Result<void>> t = kill_child(pid.value()))
            co_await t;
    }

    Result<Exited> done = Err(Error::NoMemory);
    if (Task<Result<Exited>> t = wait_child(pid.value()))
        done = co_await t;
    co_await close_fd(u32(pipe.value().r));

    if (bad.is_err())
        co_return bad;
    if (done.is_err())
        co_return Err(done.error());
    co_return done.value().status;
}

} // namespace

Task<i32> proc_main(Args args)
{
    // -m and -n in either order, up to the command's first word.
    bool milli = false, bad = false;
    Option<u32> n = None;
    usize first   = 1;
    for (; first < args.size(); first++) {
        Str a = args[first];
        if (a == "-m") {
            milli = true;
            continue;
        }
        if (a != "-n")
            break;
        if (first + 1 >= args.size()) {
            bad = true;
            break;
        }
        n = parse_u32(args[++first]);
        if (!n.has_value()) {
            co_await write_all(SYS_STDERR, "watch: -n wants a number\n");
            co_return 2;
        }
    }
    if (n.has_value() && !milli && n.value() > MAX_SECS)
        bad = true;
    if (bad || args.size() <= first) {
        co_await write_all(SYS_STDERR, "usage: watch [-m] [-n <seconds>] <command> [<arg>...]\n");
        co_return 2;
    }
    u32 ms = DEFAULT_SECS * 1000;
    if (n.has_value())
        ms = milli ? n.value() : n.value() * 1000;

    Args cmd{ args.v.subspan(first) };
    String out;
    for (;;) {
        Result<i32> r = Err(Error::NoMemory);
        if (Task<Result<i32>> t = once(cmd, out))
            r = co_await t;
        if (r.is_err()) {
            if (r.error() == Error::Cancelled)
                co_return 130;
            if (Task<void> e = errln("watch", cmd[0], r.error()))
                co_await e;
            co_return 1;
        }

        // Cleared only once there is something to put in its place, so the
        // screen never blinks empty while the command runs.
        if ((co_await sys_call(Sys::ScreenClear, 0)).is_err())
            co_return 1;
        if ((co_await write_all(SYS_STDOUT, out.str())).is_err())
            co_return 1;

        Result<void> slept = Err(Error::NoMemory);
        if (Task<Result<void>> t = sleep_for(ms))
            slept = co_await t;
        if (slept.is_err())
            co_return 130; // ^C, which is the only way out of this loop
    }
}

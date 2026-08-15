#include "shell.h"

#include "edit.h"
#include "job.h"
#include "kernel/fmt.h"
#include "kernel/string.h"

namespace {

// A POD, so it needs no __cxa_atexit (Concept.md §C.3) — the rule holds inside
// a process exactly as it does in the kernel.
bool g_exit_wanted;
i32 g_exit_status;

} // namespace

void shell_exit(i32 status)
{
    g_exit_wanted = true;
    g_exit_status = status;
}

bool shell_exit_wanted(i32 &status)
{
    if (!g_exit_wanted)
        return false;
    status        = g_exit_status;
    g_exit_wanted = false;
    return true;
}

namespace {

void prompt_for(Buf<16> &b, i32 status)
{
    // A nonzero status shows up here rather than in a diagnostic line: it costs
    // nothing when everything works, and a pipeline's status is its last
    // command's, so nothing about it changes.
    if (status)
        b.put('[').put(status).put("] ");
    b.put("$ ");
}

// The keyboard is ours for as long as we are at a prompt; run_line hands it to
// a foreground pipeline and takes it back. Holding it is also what makes ^C an
// ordinary key here, which is how a line is abandoned rather than the shell.
Task<i32> interactive()
{
    Task<Result<Geometry>> claim = keys_claim(true);
    if (!claim)
        co_return 1;
    if ((co_await claim).is_err()) {
        co_await write_all(SYS_STDERR, "sh: no keyboard\n");
        co_return 1;
    }

    LineEditor ed;
    i32 status = 0;

    for (;;) {
        // A background job that has finished is announced here rather than
        // wherever it happened to end, which would land in the middle of a line
        // being typed.
        if (Task<void> t = jobs_report(SYS_STDOUT))
            co_await t;

        Buf<16> prompt;
        prompt_for(prompt, status);

        Task<Result<Line>> t = ed.read_line(prompt.str());
        Result<Line> r       = t ? co_await t : Err(Error::NoMemory);
        if (r.is_err())
            co_return r.error() == Error::Cancelled ? 0 : 1;

        Line line = move(r.value());
        if (Task<Result<void>> nl = write_all(SYS_STDOUT, "\n"))
            co_await nl;

        if (line.how == LineEnd::Interrupt) {
            status = 130; // 128 + SIGINT, by convention
            continue;
        }

        Task<i32> run = run_line(line.text.str(), true);
        status        = run ? co_await run : 1;

        if (shell_exit_wanted(status))
            co_return status;
    }
}

// A shell with no console: lines off stdin, no keyboard, no foreground. What
// `sh -s` is, and what a script would be.
Task<i32> script()
{
    Input in(Args{}, SYS_STDIN);
    if (i32 bad = co_await in.open_all("sh"))
        co_return bad;

    LineReader lines(in);
    String line;
    i32 status = 0;

    for (;;) {
        Task<Result<bool>> t = lines.next(line);
        Result<bool> r       = t ? co_await t : Err(Error::NoMemory);
        if (r.is_err())
            co_return r.error() == Error::Cancelled ? 130 : 1;
        if (!r.value())
            break;

        Task<i32> run = run_line(line.str(), false);
        status        = run ? co_await run : 1;
        if (shell_exit_wanted(status))
            break;
    }
    co_return status;
}

} // namespace

Task<i32> shell(bool want_console)
{
    co_return want_console ? co_await interactive() : co_await script();
}

#include "shell.h"

#include "edit.h"
#include "fs/path.h"
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

// A nonzero status shows up here rather than in a diagnostic line: it costs
// nothing when everything works, and a pipeline's status is its last command's,
// so nothing about it changes. It is its own piece of the prompt because it is
// its own colour (edit.h).
void prompt_for(Buf<16> &b, i32 status)
{
    if (status)
        b.put('[').put(status).put("] ");
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

        Buf<16> failed;
        prompt_for(failed, status);

        // Asked for rather than remembered: cd is a builtin, but nothing says
        // it is the only thing that ever moved us. The basename points into
        // cwd, which outlives the read_line that draws it.
        String cwd;
        Str dir;
        if (Task<Result<String>> t = cwd_get())
            if (Result<String> r = co_await t; r.is_ok()) {
                cwd = move(r.value());
                dir = path_basename(cwd.str());
            }

        // Sized rather than pointed at: a literal picked at run time would ask
        // for strlen, which nothing here provides.
        Str text = dir.empty() ? "$ "_s : " $ "_s;

        Task<Result<Line>> t = ed.read_line(Prompt{ failed.str(), dir, text });
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

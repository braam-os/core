#include "shell.h"

#include "edit.h"
#include "fs/path.h"
#include "job.h"
#include "kernel/fmt.h"
#include "kernel/string.h"
#include "proc/rt.h"
#include "var.h"

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
    String acc; // what has been typed of a construct that is not finished
    i32 status = 0;

    for (;;) {
        Buf<16> failed;
        String cwd, ps2;
        Str dir;
        Prompt p;

        if (acc.empty()) {
            // A background job that has finished is announced here rather than
            // wherever it happened to end, which would land in the middle of a
            // line being typed.
            if (Task<void> t = jobs_report(SYS_STDOUT))
                co_await t;

            prompt_for(failed, status);

            // Asked for rather than remembered: cd is a builtin, but nothing
            // says it is the only thing that ever moved us. The basename points
            // into cwd, which outlives the read_line that draws it.
            if (Task<Result<String>> t = cwd_get())
                if (Result<String> r = co_await t; r.is_ok()) {
                    cwd = move(r.value());
                    dir = path_basename(cwd.str());
                }

            // Sized rather than pointed at: a literal picked at run time would
            // ask for strlen, which nothing here provides.
            p = Prompt{ failed.str(), dir, dir.empty() ? "$ "_s : " $ "_s };
        } else {
            // Copied, because the table it comes from moves under a Str.
            Str want;
            if (!var_get("PS2", want))
                want = "> ";
            if (!ps2.assign(want))
                co_return 1;
            p = Prompt{ {}, {}, ps2.str() };
        }

        Task<Result<Line>> t = ed.read_line(p);
        Result<Line> r       = t ? co_await t : Err(Error::NoMemory);
        if (r.is_err())
            co_return r.error() == Error::Cancelled ? 0 : 1;

        Line line = move(r.value());
        if (Task<Result<void>> nl = write_all(SYS_STDOUT, "\n"))
            co_await nl;

        if (line.how == LineEnd::Interrupt) {
            acc.clear();  // a half-typed construct goes with the line
            status = 130; // 128 + SIGINT, by convention
            var_status(status);
            continue;
        }

        if ((!acc.empty() && !acc.push('\n')) || !acc.append(line.text.str())) {
            co_await write_all(SYS_STDERR, "sh: out of memory\n");
            acc.clear();
            status = 1;
            continue;
        }

        // Read on rather than complain: the text ended inside something.
        if (line_incomplete(acc.str()))
            continue;

        Task<i32> run = run_line(acc.str(), true);
        status        = run ? co_await run : 1;
        acc.clear();

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
    String line, acc;
    i32 status   = 0;
    bool exiting = false;

    for (;;) {
        Task<Result<bool>> t = lines.next(line);
        Result<bool> r       = t ? co_await t : Err(Error::NoMemory);
        if (r.is_err())
            co_return r.error() == Error::Cancelled ? 130 : 1;
        if (!r.value())
            break;

        if ((!acc.empty() && !acc.push('\n')) || !acc.append(line.str()))
            co_return 1;
        // A line at a time and never slurped, so `producer | sh -s` streams.
        if (line_incomplete(acc.str()))
            continue;

        Task<i32> run = run_line(acc.str(), false);
        status        = run ? co_await run : 1;
        acc.clear();
        if (shell_exit_wanted(status)) {
            exiting = true;
            break;
        }
    }

    // Unfinished at end of input: run it anyway, so it reports its own error.
    if (!exiting && !acc.empty()) {
        Task<i32> run = run_line(acc.str(), false);
        status        = run ? co_await run : 1;
        shell_exit_wanted(status);
    }
    co_return status;
}

// A text with no reader behind it: parsed once and run once, which is what `.`
// does. shell_exit_wanted is what makes an `exit 3` inside it 3.
Task<i32> run_once(Str text)
{
    Task<i32> run = run_line(text, false);
    i32 status    = run ? co_await run : 1;
    shell_exit_wanted(status);
    co_return status;
}

// `sh <file>`. Read whole rather than a line at a time, so the script's stdin
// stays the shell's and a `read` in it reads that.
Task<i32> from_file(Str path)
{
    Result<String> text = Err(Error::NoMemory);
    if (Task<Result<String>> t = read_file(path))
        text = co_await t;
    if (text.is_err()) {
        if (text.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("sh", path, text.error()))
            co_await e;
        co_return text.error() == Error::NotFound ? 127 : 1;
    }

    Task<i32> t = run_once(text.value().str());
    co_return t ? co_await t : 1;
}

} // namespace

Task<i32> shell(ShellStart s)
{
    // Planted here, so var.cpp needs no syscall. $$ is this process's pid, so
    // a top-level /bin/sh reports init's (Concept.md §4). var_init first:
    // args_set carries $0 over from what is already there.
    if (!var_init(proc_pid(), s.name0) || !args_set(s.args)) {
        co_await write_all(SYS_STDERR, "sh: out of memory\n");
        co_return 1;
    }
    sh_set_flags(s.flags);

    bool console = s.from == ShellIn::Console;
    Task<i32> t;
    switch (s.from) {
    case ShellIn::Console:
        t = interactive();
        break;
    case ShellIn::Stdin:
        t = script();
        break;
    case ShellIn::File:
        t = from_file(s.text);
        break;
    case ShellIn::Command:
        t = run_once(s.text);
        break;
    }
    i32 status = t ? co_await t : 1;

    // `trap … 0`, whatever ended the loop. Its own status is not the shell's.
    Str action;
    if (trap_get(0, action))
        if (Task<i32> e = trap_run(0, console))
            co_await e;
    co_return status;
}

#include "shell.h"

#include "boot.h"
#include "edit.h"
#include "job.h"
#include "kernel/fmt.h"
#include "kernel/screen.h"
#include "kernel/traits.h"
#include "tty.h"

namespace {

// A POD, so it needs no __cxa_atexit (Concept.md §C.3).
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

// A plain function rather than a coroutine, and the screen rather than a
// Stream: the shell's frame has a size class to fit inside (test_shell guards
// it), and one more awaitable and one more local would have taken it over.
// Writing straight to the grid is what boot.cpp already does for the same
// reason, and the shell's own output has never been anywhere else.
bool finished(i32 &status)
{
    if (!shell_exit_wanted(status))
        return false;
    screen_write("braam: the shell exited — reload to start again");
    screen_newline();
    return true;
}

} // namespace

Task<i32> shell()
{
    LineEditor ed;
    Stdio io   = stdio_console();
    i32 status = 0;

    screen_cursor(true);

    // Before the first prompt, not alongside it: every line the shell can be
    // given needs the mounts already in place. The work is in a coroutine of
    // its own so that its locals stay out of this frame, which has a size
    // class to fit inside (test_shell guards it).
    if (Task<void> t = boot_filesystem())
        co_await t;

    for (;;) {
        // A background job that has finished is announced here rather than
        // wherever it happened to end, which would land in the middle of a
        // line being typed. In a coroutine of its own, like the boot mounting,
        // so that its locals stay out of this frame.
        if (Task<void> t = jobs_report(io.out))
            co_await t;

        // The prompt lives in this frame, because read_line only holds a view
        // of it. A nonzero status shows up here rather than in a diagnostic
        // line: it costs nothing when everything works, and a pipeline's
        // status is its last command's, so nothing about it changes.
        Buf<16> prompt;
        if (status)
            prompt.put('[').put(status).put("] ");
        prompt.put("$ ");

        Result<Line> r = co_await ed.read_line(prompt.str());
        if (r.is_err())
            co_return r.error() == Error::Cancelled ? 0 : 1;

        Line line = move(r.value());
        screen_newline();

        if (line.how == LineEnd::Interrupt) {
            status = 130; // 128 + SIGINT, by convention
            continue;
        }

        // Nothing borrows from line.text any more: quote removal means a word
        // is not a substring of the line, so the parser copies into a store of
        // its own, which the pipeline's own heap block owns. Running the line
        // elsewhere is also what keeps this frame inside a size class.
        status = co_await run_line(line.text.str(), io);

        // The terminal is dead after this: init spawns the shell and nothing
        // else, so there is no getty to start another. Say so rather than leave
        // a prompt that never comes back.
        // `status` doubles as the exit status: `exit` sets it, and the shell's
        // last status is what it leaves with either way.
        if (finished(status))
            co_return status;
    }
}

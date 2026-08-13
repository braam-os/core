#include "shell.h"

#include "edit.h"
#include "kernel/fmt.h"
#include "kernel/screen.h"
#include "kernel/traits.h"
#include "prog.h"
#include "tokenize.h"
#include "tty.h"

Task<i32> shell() {
    LineEditor ed;
    Stdio io = stdio_console();
    Vec<Str> argv;
    i32 status = 0;

    screen_cursor(true);

    for (;;) {
        // The prompt lives in this frame, because read_line only holds a view
        // of it. A nonzero status shows up here rather than in a diagnostic
        // line: it costs nothing when everything works, and it invents no
        // stderr semantics before M4 has defined any.
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

        // argv borrows from line.text, which stays alive across the co_await
        // below. That is the invariant M4's pipelines must not break.
        argv.clear();
        if (tokenize(line.text.str(), argv).is_err()) {
            status = 1;
            continue;
        }
        if (argv.empty()) {
            status = 0;
            continue;
        }

        const Program *prog = program_find(argv[0]);
        if (!prog) {
            co_await io.err.write("braam: ");
            co_await io.err.write(argv[0]);
            co_await io.err.write(": not found\n");
            status = 127;
            continue;
        }

        // Awaiting the child is what makes its exit code observable at all —
        // sched_spawn reaps a finished job and discards it — and it is what
        // carries the cancel state down into the program. It is also why
        // nothing else is ever parked on the keyboard while a program runs:
        // a Channel has one receiver, and this coroutine is not it.
        status = co_await prog->run(Args{argv}, io);
    }
}

#include "kernel/text.h"
#include "proc/io.h"
#include "sh/shell.h"

// The shell, as a program like any other (Concept.md §4). There is no in-kernel
// shell left to be §4's exception: what a prompt needs — a pipeline, a
// redirection, a job, a working directory, the keyboard and the cursor — is
// what the §4.3 syscall table is.
//
// `-s` reads stdin a line at a time instead of drawing a prompt. Without it the
// shell asks for the keyboard, and says so and stops if somebody else has it.
Task<i32> proc_main(Args args)
{
    bool console = true;
    for (usize i = 1; i < args.size(); i++) {
        if (args[i] == "-s") {
            console = false;
            continue;
        }
        co_await write_all(SYS_STDERR, "usage: sh [-s]\n");
        co_return 2;
    }

    Task<i32> t = shell(console);
    co_return t ? co_await t : 1;
}

#include "decl.h"
#include "kernel/text.h"
#include "user/io.h"
#include "user/shell.h"

// Asks rather than acts: a builtin is an ordinary pipeline stage, so this runs
// with the shell parked on the stage's report and there is nothing to return
// to. The shell reads the request when run_line comes back, which is also what
// makes `exit | cat` and `exit &` end the shell at the next prompt rather than
// halfway through one.
Task<i32> builtin_exit(Args args, Stdio io)
{
    if (args.size() > 2) {
        co_await io.err.write("usage: exit [<status>]\n");
        co_return 2;
    }

    i32 status = 0;
    if (args.size() == 2) {
        Option<u32> n = parse_u32(args[1]);
        if (!n) {
            co_await io.err.write("exit: not a status: ");
            co_await io.err.write(args[1]);
            co_await io.err.write("\n");
            co_return 2;
        }
        status = i32(n.value() & 0xff);
    }

    shell_exit(status);
    co_return status;
}

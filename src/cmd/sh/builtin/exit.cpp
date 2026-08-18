#include "cmd/sh/shell.h"
#include "decl.h"
#include "kernel/text.h"

// Asks rather than acts. A builtin runs with the shell in the middle of a line,
// so returning from here returns into run_line and not out of the shell; the
// loop reads the request when the line is finished. That is also what makes
// `exit | cat` and `exit &` end the shell at the next prompt rather than
// halfway through one.
Task<i32> builtin_exit(Args args, ShIo io)
{
    if (args.size() > 2) {
        co_await write_all(io.err, "usage: exit [<status>]\n");
        co_return 2;
    }

    i32 status = 0;
    if (args.size() == 2) {
        Option<u32> n = parse_u32(args[1]);
        if (!n) {
            if (Task<void> e = errln("exit", args[1], Error::Invalid))
                co_await e;
            co_return 2;
        }
        status = i32(n.value() & 0xff);
    }

    shell_exit(status);
    co_return status;
}

#include "user/prog.h"

// There is no quoting yet, so a quote is a literal character. Quoting arrives
// with M4's grammar, alongside pipes and redirection.
BRAAM_PROGRAM(prog_echo, "echo", "[-n] [word...] — write the arguments")
{
    usize i      = 1;
    bool newline = true;
    if (i < args.size() && args[i] == "-n") {
        newline = false;
        i++;
    }

    for (bool first = true; i < args.size(); i++, first = false) {
        if (!first)
            co_await io.out.write(" ");
        co_await io.out.write(args[i]);
    }
    if (newline)
        co_await io.out.write("\n");

    co_return 0;
}

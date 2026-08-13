#include "kernel/text.h"
#include "user/io.h"
#include "user/prog.h"

// Returning early is what stops the producer upstream: the stage runner hangs
// up this program's input, and the next write on the other side reports Closed.
BRAAM_PROGRAM(prog_head, "head", "[-n <count>] — the first lines, ten by default")
{
    u32 want = 10;
    if (args.size() == 3 && args[1] == "-n") {
        Option<u32> n = parse_u32(args[2]);
        if (!n.has_value()) {
            co_await io.err.write("usage: head [-n <count>]\n");
            co_return 2;
        }
        want = n.value();
    } else if (args.size() != 1) {
        co_await io.err.write("usage: head [-n <count>]\n");
        co_return 2;
    }

    LineReader in(io.in);
    String line;
    for (u32 seen = 0; seen < want; seen++) {
        Result<bool> r = co_await in.next(line);
        if (r.is_err())
            co_return r.error() == Error::Cancelled ? 130 : 1;
        if (!r.value())
            break;
        if (!line.push('\n'))
            co_return 1;
        if ((co_await write_all(io.out, line.str())).is_err())
            co_return 1;
    }

    co_return 0;
}

#include "kernel/text.h"
#include "proc/io.h"

// Returning early is what stops the producer upstream: the stage runner hangs
// up this program's input, and the next write on the other side reports Closed.
Task<i32> proc_main(Args args)
{
    u32 want    = 10;
    usize first = 1;
    if (args.size() >= 3 && args[1] == "-n") {
        Option<u32> n = parse_u32(args[2]);
        if (!n.has_value()) {
            co_await write_all(SYS_STDERR, "usage: head [-n <count>] [<file>...]\n");
            co_return 2;
        }
        want  = n.value();
        first = 3;
    } else if (args.size() >= 2 && args[1].starts_with("-")) {
        co_await write_all(SYS_STDERR, "usage: head [-n <count>] [<file>...]\n");
        co_return 2;
    }

    Input files(Args{ args.v.subspan(first) }, SYS_STDIN, "head");

    LineReader in(files);
    String line;
    for (u32 seen = 0; seen < want; seen++) {
        Result<bool> r = co_await in.next(line);
        if (r.is_err())
            co_return r.error() == Error::Cancelled ? 130 : 1;
        if (!r.value())
            break;
        if (!line.push('\n'))
            co_return 1;
        if ((co_await write_all(SYS_STDOUT, line.str())).is_err())
            co_return 1;
    }

    co_return 0;
}

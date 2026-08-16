#include "kernel/text.h"
#include "kernel/vec.h"
#include "proc/io.h"

// A ring of the last `want` lines, so the input is read once and only the
// answer is held. tail cannot stop early: the last line is the last one.
//
// The registry's tail, moved to a binary of its own (Concept.md §4). The body
// is the program it was; what changed is that its reads and writes are
// syscalls, and its ring is in sixteen megabytes that are nobody else's.
Task<i32> proc_main(Args args)
{
    u32 want    = 10;
    usize first = 1;
    if (args.size() >= 3 && args[1] == "-n") {
        Option<u32> n = parse_u32(args[2]);
        if (!n.has_value()) {
            co_await write_all(SYS_STDERR, "usage: tail [-n <count>] [<file>...]\n");
            co_return 2;
        }
        want  = n.value();
        first = 3;
    } else if (args.size() >= 2 && args[1].starts_with("-")) {
        co_await write_all(SYS_STDERR, "usage: tail [-n <count>] [<file>...]\n");
        co_return 2;
    }

    Input files(Args{ args.v.subspan(first) }, SYS_STDIN, "tail");

    Vec<String> ring;
    usize head = 0; // oldest, once the ring is full
    LineReader in(files);
    String line;

    while (want > 0) {
        Result<bool> r = co_await in.next(line);
        if (r.is_err())
            co_return r.error() == Error::Cancelled ? 130 : 1;
        if (!r.value())
            break;

        if (ring.size() < want) {
            String copy;
            if (!copy.assign(line.str()) || !ring.push(move(copy)))
                co_return 1;
            continue;
        }
        if (!ring[head].assign(line.str()))
            co_return 1;
        head = (head + 1) % ring.size();
    }

    for (usize i = 0; i < ring.size(); i++) {
        String &s = ring[(head + i) % ring.size()];
        if (!s.push('\n'))
            co_return 1;
        if ((co_await write_all(SYS_STDOUT, s.str())).is_err())
            co_return 1;
    }

    co_return 0;
}

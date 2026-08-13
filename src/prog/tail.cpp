#include "kernel/text.h"
#include "kernel/vec.h"
#include "user/io.h"
#include "user/prog.h"

// A ring of the last `want` lines, so the input is read once and only the
// answer is held. tail cannot stop early: the last line is the last one.
BRAAM_PROGRAM(prog_tail, "tail", "[-n <count>] — the last lines, ten by default")
{
    u32 want = 10;
    if (args.size() == 3 && args[1] == "-n") {
        Option<u32> n = parse_u32(args[2]);
        if (!n.has_value()) {
            co_await io.err.write("usage: tail [-n <count>]\n");
            co_return 2;
        }
        want = n.value();
    } else if (args.size() != 1) {
        co_await io.err.write("usage: tail [-n <count>]\n");
        co_return 2;
    }

    Vec<String> ring;
    usize head = 0; // oldest, once the ring is full
    LineReader in(io.in);
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
        if ((co_await write_all(io.out, s.str())).is_err())
            co_return 1;
    }

    co_return 0;
}

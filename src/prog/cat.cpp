#include "user/io.h"
#include "user/prog.h"

// Chunks, not lines: cat is byte-exact, so a last line without a newline stays
// that way. Named files are read end to end as one stream.
BRAAM_PROGRAM(prog_cat, "cat", "[<file>...] — copy files, or the input, to the output")
{
    Inputs files;
    if (i32 bad = co_await open_inputs(files, args.tail(), "cat", io))
        co_return bad;

    Source in = input_of(files, io);
    for (;;) {
        Result<String> r = co_await in.read();
        if (r.is_err()) {
            if (r.error() == Error::Closed)
                co_return 0;
            co_return r.error() == Error::Cancelled ? 130 : 1;
        }
        if ((co_await write_all(io.out, r.value().str())).is_err())
            co_return 1;
    }
}

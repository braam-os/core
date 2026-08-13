#include "user/io.h"
#include "user/prog.h"

// Chunks, not lines: cat is byte-exact, so a last line without a newline stays
// that way. File arguments arrive with M5.
BRAAM_PROGRAM(prog_cat, "cat", "copy the input to the output")
{
    if (args.size() > 1) {
        co_await io.err.write("usage: cat\n");
        co_return 2;
    }

    for (;;) {
        Result<String> r = co_await io.in.read();
        if (r.is_err()) {
            if (r.error() == Error::Closed)
                co_return 0;
            co_return r.error() == Error::Cancelled ? 130 : 1;
        }
        if ((co_await write_all(io.out, r.value().str())).is_err())
            co_return 1;
    }
}

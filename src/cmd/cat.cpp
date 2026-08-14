#include "proc/io.h"

// Chunks, not lines: cat is byte-exact, so a last line without a newline stays
// that way. Named files are read end to end as one stream.
Task<i32> proc_main(Args args)
{
    Input files(args.tail(), SYS_STDIN);
    if (i32 bad = co_await files.open_all("cat"))
        co_return bad;

    for (;;) {
        Result<String> r = co_await files.read();
        if (r.is_err()) {
            if (r.error() == Error::Closed)
                co_return 0;
            co_return r.error() == Error::Cancelled ? 130 : 1;
        }
        if ((co_await write_all(SYS_STDOUT, r.value().str())).is_err())
            co_return 1;
    }
}

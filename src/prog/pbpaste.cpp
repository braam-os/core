#include "svc/svc.h"
#include "user/io.h"
#include "user/prog.h"

BRAAM_PROGRAM(prog_pbpaste, "pbpaste", "— write the clipboard to the output")
{
    if (args.size() != 1) {
        co_await io.err.write("usage: pbpaste\n");
        co_return 2;
    }

    Result<String> got = Err(Error::NoMemory);
    if (Task<Result<String>> t = clip_read())
        got = co_await t;
    if (got.is_err()) {
        if (got.error() == Error::Cancelled)
            co_return 130;
        co_await io.err.write("pbpaste: ");
        co_await io.err.write(error_name(got.error()));
        co_await io.err.write("\n");
        co_return 1;
    }

    if ((co_await write_all(io.out, got.value().str())).is_err())
        co_return 1;
    co_return 0;
}

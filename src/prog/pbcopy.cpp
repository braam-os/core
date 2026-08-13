#include "svc/svc.h"
#include "user/io.h"
#include "user/prog.h"

// The clipboard belongs to the page, not the worker, so this crosses two
// boundaries; a browser that refuses without a user gesture reports Perm.
BRAAM_PROGRAM(prog_pbcopy, "pbcopy", "[<file>...] — put the input on the clipboard")
{
    Inputs files;
    if (i32 bad = co_await open_inputs(files, args.tail(), "pbcopy", io))
        co_return bad;

    String text;
    Source in = input_of(files, io);
    for (;;) {
        Result<String> r = co_await in.read();
        if (r.is_err()) {
            if (r.error() == Error::Closed)
                break;
            co_return r.error() == Error::Cancelled ? 130 : 1;
        }
        if (!text.append(r.value().str()))
            co_return 1;
    }

    Result<void> put = Err(Error::NoMemory);
    if (Task<Result<void>> t = clip_write(text.str()))
        put = co_await t;
    if (put.is_err()) {
        if (put.error() == Error::Cancelled)
            co_return 130;
        co_await io.err.write("pbcopy: ");
        co_await io.err.write(error_name(put.error()));
        co_await io.err.write("\n");
        co_return 1;
    }
    co_return 0;
}

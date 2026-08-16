#include "proc/io.h"

// The clipboard belongs to the page, not the worker, so this crosses two
// boundaries; a browser that refuses without a user gesture reports Perm.
Task<i32> proc_main(Args args)
{
    Input files(args.tail(), SYS_STDIN, "pbcopy");

    String text;
    for (;;) {
        Result<String> r = co_await files.read();
        if (r.is_err()) {
            if (r.error() == Error::Closed)
                break;
            co_return r.error() == Error::Cancelled ? 130 : 1;
        }
        if (!text.append(r.value().str()))
            co_return 1;
    }

    Result<void> put = Err(Error::NoMemory);
    if (Task<Result<void>> t = clip_put(text.str()))
        put = co_await t;
    if (put.is_err()) {
        if (put.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("pbcopy", Str(), put.error()))
            co_await e;
        co_return 1;
    }
    co_return 0;
}

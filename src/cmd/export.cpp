#include "fs/path.h"
#include "proc/io.h"

// The way out (Concept.md §5.4): the file becomes a Blob and the browser
// downloads it. The whole file is buffered, because a download is one Blob.
Task<i32> proc_main(Args args)
{
    if (args.size() < 2 || args.size() > 3) {
        co_await write_all(SYS_STDERR, "usage: export <file> [<name>]\n");
        co_return 2;
    }

    Input files(Args{ args.v.subspan(1, 1) }, SYS_STDIN, "export");

    String data;
    for (;;) {
        Result<String> r = co_await files.read();
        if (r.is_err()) {
            if (r.error() == Error::Closed)
                break;
            co_return r.error() == Error::Cancelled ? 130 : 1;
        }
        if (!data.append(r.value().str()))
            co_return 1;
    }

    Str name          = args.size() == 3 ? args[2] : path_basename(args[1]);
    Result<void> done = Err(Error::NoMemory);
    if (Task<Result<void>> t = save(name, data.str()))
        done = co_await t;
    if (done.is_err()) {
        if (done.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("export", name, done.error()))
            co_await e;
        co_return 1;
    }
    co_return 0;
}

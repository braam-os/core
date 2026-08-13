#include "fs/path.h"
#include "svc/xfer.h"
#include "user/io.h"
#include "user/prog.h"

// The way out (Concept.md §5.4): the file becomes a Blob and the browser
// downloads it. The whole file is buffered, because a download is one Blob.
BRAAM_PROGRAM(prog_export, "export", "<file> [<name>] — download a file out of the browser")
{
    if (args.size() < 2 || args.size() > 3) {
        co_await io.err.write("usage: export <file> [<name>]\n");
        co_return 2;
    }

    Inputs files;
    if (i32 bad = co_await open_inputs(files, Args{ args.v.subspan(1, 1) }, "export", io))
        co_return bad;

    String data;
    Source in = files.source();
    for (;;) {
        Result<String> r = co_await in.read();
        if (r.is_err()) {
            if (r.error() == Error::Closed)
                break;
            co_return r.error() == Error::Cancelled ? 130 : 1;
        }
        if (!data.append(r.value().str()))
            co_return 1;
    }

    Str name           = args.size() == 3 ? args[2] : path_basename(args[1]);
    Result<void> saved = Err(Error::NoMemory);
    if (Task<Result<void>> t = save_file(name, data.str()))
        saved = co_await t;
    if (saved.is_err()) {
        if (saved.error() == Error::Cancelled)
            co_return 130;
        co_await io.err.write("export: ");
        co_await io.err.write(name);
        co_await io.err.write(": ");
        co_await io.err.write(error_name(saved.error()));
        co_await io.err.write("\n");
        co_return 1;
    }
    co_return 0;
}

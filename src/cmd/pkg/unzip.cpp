#include "unzip.h"

#include "proc/io.h"

Task<Result<String>> zip_read(const ZipEntry &e)
{
    if (e.method == ZIP_STORE) {
        Str bytes;
        if (!zip_stored(e, bytes))
            co_return Err(Error::Invalid);
        String out;
        if (!out.assign(bytes))
            co_return Err(Error::NoMemory);
        co_return move(out);
    }
    if (e.method != ZIP_DEFLATE || e.data.size() > ZIP_PACKED_MAX)
        co_return Err(Error::Unsupported);

    Result<i32> open = Err(Error::NoMemory);
    if (Task<Result<i32>> t = inflate(e.data))
        open = co_await t;
    if (open.is_err())
        co_return Err(open.error());
    u32 fd = u32(open.value());

    ZipSink sink(e.size);
    Error failure = Error::Invalid;
    bool ok       = false;
    for (;;) {
        Result<String> chunk = Err(Error::NoMemory);
        if (Task<Result<String>> t = read_chunk(fd))
            chunk = co_await t;
        if (chunk.is_err()) {
            if (chunk.error() == Error::Closed)
                ok = sink.complete();
            else
                failure = chunk.error();
            break;
        }
        if (!sink.take(chunk.value().str()))
            break; // past the declared size: a bomb, abandoned part way
    }

    if (Task<void> t = close_fd(fd))
        co_await t;
    if (!ok)
        co_return Err(failure);
    co_return move(sink.text());
}

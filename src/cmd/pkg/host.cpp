#include "host.h"

#include "kernel/fmt.h"
#include "kernel/traits.h"
#include "pkg.h"
#include "proc/io.h"

// Every fetch /bin/pkg makes passes through here, so -v is traced here and
// nowhere else: update, install and upgrade at once, and none of it in the half
// that compiles into tests.wasm.

namespace {

// `> ` is what was asked for, `< ` what came back — curl's two directions, on
// stderr so a trace never lands in a pipe that wanted the output.
Task<void> trace(Str lead, Str text)
{
    Buf<128> b;
    b.put(lead).put(text).put('\n');
    if (Task<Result<void>> t = write_all(SYS_STDERR, b.str()))
        co_await t;
}

// The response headers as web/svc.js packed them: `name: value` a line. A
// cross-origin response carries only the CORS-safelisted ones.
Task<void> trace_headers(Str headers)
{
    Str rest = headers, line;
    while (next_line(rest, line))
        if (!line.empty())
            if (Task<void> t = trace("< ", line))
                co_await t;
}

struct ProcHost : PkgHost {
    // One body is open at a time: fetch_capped opens, drains and closes before
    // the next url.
    u64 read_bytes = 0;

    Task<Result<u64>> now() override
    {
        Task<Result<Clock>> t = clock_now();
        if (!t)
            co_return Err(Error::NoMemory);
        Result<Clock> r = co_await t;
        if (r.is_err())
            co_return Err(r.error());
        co_return r.value().epoch_ms;
    }

    Task<Result<String>> load(Str path) override
    {
        Task<Result<String>> t = read_file(path);
        if (!t)
            co_return Err(Error::NoMemory);
        co_return co_await t;
    }

    Task<Result<i32>> open(Str url, u32 &status) override
    {
        // GET, no headers, no body. The request is the whole of what a page
        // may say it sent: the headers the browser adds are its own.
        read_bytes = 0;
        if (pkg_verbose())
            if (Task<void> t = trace("> GET ", url))
                co_await t;

        Task<Result<Fetched>> t = fetch_url(url, "\n\n");
        if (!t)
            co_return Err(Error::NoMemory);
        Result<Fetched> r = co_await t;
        if (r.is_err()) {
            if (pkg_verbose())
                if (Task<void> e = trace("< no response: ", error_name(r.error())))
                    co_await e;
            co_return Err(r.error());
        }
        status = r.value().status;

        if (pkg_verbose()) {
            Buf<16> line;
            line.put(status);
            if (Task<void> t2 = trace("< ", line.str()))
                co_await t2;
            if (Task<void> t2 = trace_headers(r.value().headers.str()))
                co_await t2;
        }
        co_return r.value().body;
    }

    Task<Result<String>> read(i32 body) override
    {
        Task<Result<String>> t = read_chunk(u32(body));
        if (!t)
            co_return Err(Error::NoMemory);
        Result<String> r = co_await t;
        if (r.is_ok())
            read_bytes += r.value().size();
        co_return move(r);
    }

    Task<void> close(i32 body) override
    {
        if (pkg_verbose()) {
            Buf<32> line;
            line.put(read_bytes).put(" bytes");
            if (Task<void> t = trace("< ", line.str()))
                co_await t;
        }
        if (Task<void> t = close_fd(u32(body)))
            co_await t;
    }

    Task<Result<bool>> verify(Str key, Str sig, Str bytes) override
    {
        Task<Result<bool>> t = verify_sig(key, sig, bytes);
        if (!t)
            co_return Err(Error::NoMemory);
        co_return co_await t;
    }
};

ProcHost the_host;

} // namespace

PkgHost &pkg_host()
{
    return the_host;
}

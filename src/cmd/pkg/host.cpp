#include "host.h"

#include "kernel/traits.h"
#include "proc/io.h"

namespace {

struct ProcHost : PkgHost {
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
        // GET, no headers, no body.
        Task<Result<Fetched>> t = fetch_url(url, "\n\n");
        if (!t)
            co_return Err(Error::NoMemory);
        Result<Fetched> r = co_await t;
        if (r.is_err())
            co_return Err(r.error());
        status = r.value().status;
        co_return r.value().body;
    }

    Task<Result<String>> read(i32 body) override
    {
        Task<Result<String>> t = read_chunk(u32(body));
        if (!t)
            co_return Err(Error::NoMemory);
        co_return co_await t;
    }

    Task<void> close(i32 body) override
    {
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

#include "anchor.h"

#include "kernel/traits.h"
#include "proc/io.h"

namespace {

Task<Result<bool>> verify_here(Str key, Str sig, Str bytes)
{
    Task<Result<bool>> t = verify_sig(key, sig, bytes);
    if (!t)
        co_return Err(Error::NoMemory);
    co_return co_await t;
}

} // namespace

Task<Result<void>> anchor_load(u64 now, AnchorFile &out)
{
    Result<String> text = Err(Error::NoMemory);
    if (Task<Result<String>> t = read_file(ANCHOR_PATH))
        text = co_await t;
    if (text.is_err())
        co_return Err(text.error());

    if (!anchor_file_read(move(text.value()), out))
        co_return Err(Error::Invalid);

    Result<bool> ok = Err(Error::NoMemory);
    if (Task<Result<bool>> t = trust_self(out, now, verify_here))
        ok = co_await t;
    if (ok.is_err())
        co_return Err(ok.error());
    co_return ok.value() ? Result<void>() : Err(Error::Perm);
}

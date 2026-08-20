#include "kernel/fmt.h"
#include "proc/io.h"
#include "proc/opt.h"

namespace {

constexpr Str USAGE = "usage: mkdir [-p] <dir>...\n";

} // namespace

Task<i32> proc_main(Args args)
{
    bool parents = false;
    OptParse p(args, Opts{ "p", "" });
    for (Opt o;;) {
        Result<bool> r = p.next(o);
        if (r.is_err()) {
            Buf<64> b;
            b.put("mkdir: illegal option -- ").put(o.name).put('\n');
            co_await write_all(SYS_STDERR, b.str());
            co_await write_all(SYS_STDERR, USAGE);
            co_return 2;
        }
        if (!r.value())
            break;
        parents = true;
    }

    Args rest = p.rest();
    if (rest.size() == 0) {
        co_await write_all(SYS_STDERR, USAGE);
        co_return 2;
    }

    i32 status = 0;
    for (usize i = 0; i < rest.size(); i++) {
        Result<void> r = Err(Error::NoMemory);
        if (Task<Result<void>> t = parents ? make_dir_all(rest[i]) : make_dir(rest[i]))
            r = co_await t;
        if (r.is_ok())
            continue;
        if (r.error() == Error::Cancelled)
            co_return 130;

        status = 1;
        if (Task<void> e = errln("mkdir", rest[i], r.error()))
            co_await e;
    }
    co_return status;
}

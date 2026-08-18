#include "cmd/sh/job.h"
#include "decl.h"
#include "kernel/text.h"

// Asks rather than acts, as `break` and `exit` do: the function call that must
// hear it is up the walk. Outside one it is a no-op.
Task<i32> builtin_return(Args args, ShIo io)
{
    if (args.size() > 2) {
        co_await write_all(io.err, "usage: return [<status>]\n");
        co_return 2;
    }

    i32 status = 0;
    if (args.size() == 2) {
        Option<u32> n = parse_u32(args[1]);
        if (!n) {
            if (Task<void> e = errln("return", args[1], Error::Invalid))
                co_await e;
            co_return 2;
        }
        status = i32(n.value() & 0xff);
    }

    func_return(status);
    co_return status;
}

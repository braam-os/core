#include "kernel/version.h"

#include "kernel/fmt.h"
#include "proc/io.h"

// The version is compiled in rather than asked for: a binary and the kernel it
// runs on are built together, and PROC_ABI is what catches the case where they
// were not.
Task<i32> proc_main(Args)
{
    Buf<48> b;
    b.put("braam ").put(BRAAM_VERSION).put('\n');
    if ((co_await write_all(SYS_STDOUT, b.str())).is_err())
        co_return 1;
    co_return 0;
}

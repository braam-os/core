#include "kernel/version.h"

#include "kernel/fmt.h"
#include "user/io.h"
#include "user/prog.h"

BRAAM_PROGRAM(prog_version, "version", "print the kernel version")
{
    Buf<48> b;
    b.put("braam ").put(BRAAM_VERSION).put('\n');
    if ((co_await write_all(io.out, b.str())).is_err())
        co_return 1;
    co_return 0;
}

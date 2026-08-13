#include "kernel/version.h"

#include "user/prog.h"

BRAAM_PROGRAM(prog_version, "version", "print the kernel version")
{
    co_await io.out.write("braam ");
    co_await io.out.write(BRAAM_VERSION);
    co_await io.out.write("\n");
    co_return 0;
}

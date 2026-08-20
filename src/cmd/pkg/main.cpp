#include "pkg.h"

// The package manager, as a program like any other. Everything it does it does
// over the §4.3 syscall table; the kernel learns none of /pkg's file formats.
Task<i32> proc_main(Args args)
{
    Task<i32> t = pkg_run(args);
    co_return t ? co_await t : 1;
}

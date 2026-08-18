#include "cmd/sh/condrun.h"
#include "proc/io.h"

// The same expression `/bin/sh`'s builtin evaluates, over the same two files.
// The builtin shadows the name at a prompt; this is what anything spawning by
// path gets, which is what a future `find -exec` needs.
//
// One name, not two: v7 links the binary again as `[`, and here `[` is a row
// in the shell's table rather than a second file.
Task<i32> proc_main(Args args)
{
    Task<i32> t = cond_run(args.tail(), SYS_STDERR);
    co_return t ? co_await t : 1;
}

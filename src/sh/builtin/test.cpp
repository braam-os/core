#include "decl.h"
#include "sh/condrun.h"

// Not the shell's own state: builtin.h's second clause. The expression itself
// is ../cond.cpp, and /bin/test runs the same two files.
Task<i32> builtin_test(Args args, ShIo io)
{
    Task<i32> t = cond_run(args.tail(), io.err);
    co_return t ? co_await t : 1;
}

// v7 links the binary a second time under this name; here it is a second row
// in the table. The closing bracket is still required, and discarded.
Task<i32> builtin_bracket(Args args, ShIo io)
{
    if (args.size() < 2 || args[args.size() - 1] != "]") {
        co_await write_all(io.err, "test: ] missing\n");
        co_return 2;
    }

    Task<i32> t = cond_run(Args{ args.v.subspan(1, args.size() - 2) }, io.err);
    co_return t ? co_await t : 1;
}

#include "kernel/fmt.h"
#include "user/io.h"
#include "user/prog.h"

// The registry is the only namespace there is until M5 brings a filesystem,
// and it is what /bin will hold when there is one — so listing it is what `ls`
// means here. M5 replaces the body with a walk of the mount table.
BRAAM_PROGRAM(prog_ls, "ls", "list the programs (there is no filesystem yet)")
{
    if (args.size() > 1) {
        co_await io.err.write("usage: ls\n");
        co_return 2;
    }

    for (const Program *p = program_first(); p; p = p->next) {
        Buf<64> b;
        b.put(p->name).put('\n');
        if ((co_await write_all(io.out, b.str())).is_err())
            co_return 1;
    }

    co_return 0;
}

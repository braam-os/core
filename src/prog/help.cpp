#include "kernel/fmt.h"
#include "user/io.h"
#include "user/prog.h"

// Two passes over the registry: the widest name, then the padded listing. The
// registry is kept sorted, so this needs no sort of its own.
BRAAM_PROGRAM(prog_help, "help", "list the programs")
{
    usize width = 0;
    for (const Program *p = program_first(); p; p = p->next)
        if (p->name.size() > width)
            width = p->name.size();

    for (const Program *p = program_first(); p; p = p->next) {
        Buf<128> b;
        b.put("  ").put(p->name);
        for (usize k = p->name.size(); k < width; k++)
            b.put(' ');
        b.put("  ").put(p->usage).put('\n');
        if ((co_await write_all(io.out, b.str())).is_err())
            co_return 1;
    }

    co_return 0;
}

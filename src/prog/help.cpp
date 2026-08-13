#include "kernel/fmt.h"
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
        co_await io.out.write(b.str());
    }

    co_return 0;
}

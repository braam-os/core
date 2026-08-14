#include "fs/vfs.h"
#include "kernel/fmt.h"
#include "user/io.h"
#include "user/prog.h"

// Two passes over the registry: the widest name, then the padded listing. The
// registry is kept sorted, so this needs no sort of its own.
//
// /usr/bin follows, because since M8 a program need not be in the registry to
// be a program (Concept.md §4). What tier a name runs at is not something the
// listing says: that is the point of the tier being the binary's business.
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

    Result<Vec<Entry>> bin = Err(Error::NotFound);
    if (Task<Result<Vec<Entry>>> t = vfs_list("/usr/bin"))
        bin = co_await t;
    if (bin.is_err())
        co_return 0;

    for (const Entry &e : bin.value()) {
        if (e.kind != NodeKind::File)
            continue;
        Buf<128> b;
        b.put("  ").put(e.name.str());
        for (usize k = e.name.size(); k < width; k++)
            b.put(' ');
        b.put("  a program in /usr/bin\n");
        if ((co_await write_all(io.out, b.str())).is_err())
            co_return 1;
    }

    co_return 0;
}

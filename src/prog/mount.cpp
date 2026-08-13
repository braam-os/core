#include "fs/vfs.h"
#include "kernel/fmt.h"
#include "user/io.h"
#include "user/prog.h"

// Mounting is not yet something a user does: the table is built at boot and
// showDirectoryPicker() is M6's. Listing it is what the command means for now.
BRAAM_PROGRAM(prog_mount, "mount", "list the mounted filesystems")
{
    if (args.size() > 1) {
        co_await io.err.write("usage: mount\n");
        co_return 2;
    }

    for (const Mount &m : vfs_mounts()) {
        Buf<96> b;
        b.put(m.prefix.str()).put(" — ").put(m.fs->kind());
        b.put(m.fs->writable() ? " (rw)\n" : " (ro)\n");
        if ((co_await write_all(io.out, b.str())).is_err())
            co_return 1;
    }
    co_return 0;
}

#include "fs/hostfs.h"
#include "fs/vfs.h"
#include "kernel/fmt.h"
#include "user/io.h"
#include "user/prog.h"

// Concept.md §5.3: browser storage semantics are invisible from the outside,
// so the system reports them from the inside. Persistent versus best-effort is
// the number that actually matters — best-effort data can be evicted without
// anyone being asked (§A.2).
BRAAM_PROGRAM(prog_df, "df", "report storage quota, usage and durability")
{
    if (args.size() > 1) {
        co_await io.err.write("usage: df\n");
        co_return 2;
    }

    StorageBackend b;
    bool known = false;
    if (Task<Result<StorageBackend>> t = storage_info()) {
        Result<StorageBackend> r = co_await t;
        if (r.is_ok()) {
            b     = r.value();
            known = true;
        }
    }

    Buf<128> head;
    head.put("backend   ").put(b.opfs ? "opfs" : "memory only");
    if (b.opfs && !b.sync)
        head.put(" (no sync handles)");
    head.put('\n');
    head.put("mode      ").put(b.persisted ? "persistent" : "best-effort").put('\n');
    if (known) {
        head.put("quota     ").put(b.quota).put(" bytes\n");
        head.put("used      ").put(b.usage).put(" bytes\n");
    } else {
        head.put("quota     unknown\n");
    }
    head.put('\n');
    if ((co_await write_all(io.out, head.str())).is_err())
        co_return 1;

    for (const Mount &m : vfs_mounts()) {
        Buf<96> line;
        line.put(m.prefix.str()).put(" — ").put(m.fs->kind());

        // Only a filesystem that holds its own bytes can answer; an OPFS mount
        // is part of the origin's usage above and has no separate number.
        u64 held = m.fs->bytes();
        if (held || !m.fs->writable() || m.fs->kind() == "memfs")
            line.put(", ").put(held).put(" bytes");
        line.put('\n');
        if ((co_await write_all(io.out, line.str())).is_err())
            co_return 1;
    }
    co_return 0;
}

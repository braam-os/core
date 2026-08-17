#include "kernel/fmt.h"
#include "kernel/text.h"
#include "proc/io.h"

// Concept.md §5.3: browser storage semantics are invisible from the outside,
// so the system reports them from the inside. Persistent versus best-effort is
// the number that actually matters — best-effort data can be evicted without
// anyone being asked (§A.2).
//
// The mount table comes from /proc, which is text; only the three numbers
// behind a host round trip need a syscall, because ProcFs generates its files
// synchronously and asking the host is not.
Task<i32> proc_main(Args args)
{
    if (args.size() > 1) {
        co_await write_all(SYS_STDERR, "usage: df\n");
        co_return 2;
    }

    StorageInfo b;
    if (Task<Result<StorageInfo>> t = storage_of()) {
        Result<StorageInfo> r = co_await t;
        if (r.is_err() && r.error() == Error::Cancelled)
            co_return 130;
        if (r.is_ok())
            b = r.value();
    }

    Buf<128> head;
    head.put("backend   ").put(b.opfs ? Str("opfs") : Str("memory only"));
    if (b.opfs && !b.sync)
        head.put(" (no sync handles)");
    head.put('\n');
    head.put("mode      ").put(b.persisted ? Str("persistent") : Str("best-effort")).put('\n');
    if (b.known) {
        head.put("quota     ").put(b.quota).put(" bytes\n");
        head.put("used      ").put(b.usage).put(" bytes\n");
    } else {
        head.put("quota     unknown\n");
    }
    head.put('\n');
    if ((co_await write_all(SYS_STDOUT, head.str())).is_err())
        co_return 1;

    Result<String> mounts = Err(Error::NoMemory);
    if (Task<Result<String>> t = read_file("/proc/mounts"))
        mounts = co_await t;
    if (mounts.is_err()) {
        if (mounts.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("df", "/proc/mounts", mounts.error()))
            co_await e;
        co_return 1;
    }

    Str rest = mounts.value().str(), line;
    while (next_line(rest, line)) {
        Str prefix = next_field(line);
        Str kind   = next_field(line);
        Str mode   = next_field(line);
        Str bytes  = next_field(line);
        if (prefix.empty())
            continue;

        Buf<96> out;
        out.put(prefix).put(" — ").put(kind);

        // Only a filesystem that holds its own bytes can answer; an OPFS mount
        // is part of the origin's usage above and has no separate number.
        Option<u32> held = parse_u32(bytes);
        u32 n            = held ? held.value() : 0;
        if (n || mode == "ro")
            out.put(", ").put(u64(n)).put(" bytes");
        out.put('\n');
        if ((co_await write_all(SYS_STDOUT, out.str())).is_err())
            co_return 1;
    }
    co_return 0;
}

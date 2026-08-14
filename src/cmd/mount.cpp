#include "kernel/fmt.h"
#include "proc/io.h"

// Mounting is not yet something a user does: the table is built at boot. What
// the command means for now is listing it — and the kernel already publishes
// that as text, so this reformats /proc/mounts rather than asking for an
// operation of its own.
Task<i32> proc_main(Args args)
{
    if (args.size() > 1) {
        co_await write_all(SYS_STDERR, "usage: mount\n");
        co_return 2;
    }

    Result<String> r = Err(Error::NoMemory);
    if (Task<Result<String>> t = read_file("/proc/mounts"))
        r = co_await t;
    if (r.is_err()) {
        if (r.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("mount", "/proc/mounts", r.error()))
            co_await e;
        co_return 1;
    }

    Str rest = r.value().str(), line;
    while (next_line(rest, line)) {
        Str prefix = next_field(line);
        Str kind   = next_field(line);
        Str mode   = next_field(line);
        if (prefix.empty())
            continue;

        Buf<96> b;
        b.put(prefix).put(" — ").put(kind);
        b.put(mode == "rw" ? Str(" (rw)\n") : Str(" (ro)\n"));
        if ((co_await write_all(SYS_STDOUT, b.str())).is_err())
            co_return 1;
    }
    co_return 0;
}

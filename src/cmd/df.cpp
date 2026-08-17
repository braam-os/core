#include "kernel/fmt.h"
#include "kernel/text.h"
#include "proc/io.h"

// The mount table (Concept.md §5.3), as BSD prints it. The mounts come from
// /proc, which is text; the quota and the usage are a syscall, because ProcFs
// generates its files synchronously and asking the host is not.
//
// The backend and the durability are on the boot banner instead: neither is a
// per-mount fact, and boot starts nothing without OPFS.

namespace {

// BSD's column widths, which put a row in sixty columns. The capacity value is
// seven wide with four spaces after it; its header is nine wide with two.
constexpr usize W_NAME = 10, W_FIRST = 11, W_SIZE = 9, W_CAP = 7;

// Kibibyte blocks.
u64 blocks_of(u64 bytes)
{
    return bytes / 1024;
}

// 10G, 121M, 512B — one decimal below ten.
void human(Buf<32> &b, u64 bytes)
{
    constexpr Str UNIT[] = { "B", "K", "M", "G", "T", "P" };
    usize u              = 0;
    u64 whole = bytes, rem = 0;
    while (whole >= 1024 && u + 1 < sizeof(UNIT) / sizeof(UNIT[0])) {
        rem = whole % 1024;
        whole /= 1024;
        u++;
    }
    b.put(whole);
    if (u && whole < 10) {
        u64 tenth = (rem * 10) / 1024;
        if (tenth)
            b.put('.').put(tenth);
    }
    b.put(UNIT[u]);
}

// A -h size column.
void put_human(Buf<128> &out, u64 bytes, usize w)
{
    Buf<32> t;
    human(t, bytes);
    out.put_right(t.str(), w);
}

} // namespace

Task<i32> proc_main(Args args)
{
    bool human_sizes = false;
    if (args.size() > 2 || (args.size() == 2 && args[1] != "-h")) {
        co_await write_all(SYS_STDERR, "usage: df [-h]\n");
        co_return 2;
    }
    if (args.size() == 2)
        human_sizes = true;

    StorageInfo b;
    if (Task<Result<StorageInfo>> t = storage_of()) {
        Result<StorageInfo> r = co_await t;
        if (r.is_err() && r.error() == Error::Cancelled)
            co_return 130;
        if (r.is_ok())
            b = r.value();
    }

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

    Buf<128> head;
    head.put_left("Filesystem", W_NAME);
    head.put_right(human_sizes ? Str("Size") : Str("1K-blocks"), W_FIRST);
    head.put_right("Used", W_SIZE).put_right("Avail", W_SIZE);
    head.put_right("Capacity", W_CAP + 2).put("  Mounted on\n");
    if ((co_await write_all(SYS_STDOUT, head.str())).is_err())
        co_return 1;

    Str rest = mounts.value().str(), line;
    while (next_line(rest, line)) {
        Str prefix = next_field(line);
        Str kind   = next_field(line);
        next_field(line); // rw|ro, which `mount` is the command for
        Str bytes = next_field(line);
        if (prefix.empty())
            continue;

        // An OPFS mount is part of the origin's usage and has no figure of its
        // own, so it reports the origin's. Any other filesystem answers for
        // what it holds, all of it in use.
        bool opfs = kind == "opfs";
        u64 total = 0, used = 0;
        bool known = true;
        if (opfs) {
            known = b.known;
            total = b.quota;
            used  = b.usage > b.quota ? b.quota : b.usage;
        } else {
            Option<u32> held = parse_u32(bytes);
            total = used = held ? held.value() : 0;
        }

        // Blocks, not bytes, decide what is left: the three columns add up
        // however the division rounded.
        u64 tb = blocks_of(total), ub = blocks_of(used);

        Buf<128> out;
        out.put_left(kind, W_NAME);
        if (!known) {
            out.put_right("-", W_FIRST).put_right("-", W_SIZE).put_right("-", W_SIZE);
        } else if (human_sizes) {
            put_human(out, total, W_FIRST);
            put_human(out, used, W_SIZE);
            put_human(out, total - used, W_SIZE);
        } else {
            out.put_right(tb, W_FIRST).put_right(ub, W_SIZE).put_right(tb - ub, W_SIZE);
        }

        // Truncated, as BSD's is: 55.87% reads 55%. Nothing to divide by is
        // not nought per cent.
        if (!known || !tb) {
            out.put_right("-", W_CAP);
        } else {
            Buf<32> cap;
            cap.put(ub * 100 / tb).put('%');
            out.put_right(cap.str(), W_CAP);
        }
        out.put("    ").put(prefix).put('\n');
        if ((co_await write_all(SYS_STDOUT, out.str())).is_err())
            co_return 1;
    }
    co_return 0;
}

#include "kernel/fmt.h"
#include "kernel/text.h"
#include "proc/io.h"

// The counters in /proc/stat as rates, the way BSD's vmstat prints them: one
// open per row, the first row averaged over uptime and each later one over the
// interval since the last. `alloc` stands where BSD's `page` group did, and
// `-loop-` where its `cpu` group did; there is no paging and nothing meters a
// CPU (Concept.md §4.2, §8.2).
//
// `r` and `p` are never nought while vmstat is the one asking: its own syscall
// server is runnable and its stepper is parked on the reply.

namespace {

// A row inside eighty columns. Memory is KB rather than `ps`'s scaled units.
constexpr usize W_PROC = 3, W_MEM = 7, W_AL = 6, W_FR = 6, W_GR = 4;
constexpr usize W_RATE = 5, W_TICK = 6;
constexpr usize GAP = 2;

// Seconds, as BSD's are — the one time argument here that is not milliseconds.
constexpr u32 DEFAULT_SECS = 1;
constexpr u32 MAX_SECS     = 4294967; // as many as convert to ms inside a u32

constexpr usize FALLBACK_ROWS = 20; // what BSD assumes without a window size

// Everything a row needs, in the order the file is read.
struct Sample {
    u64 now   = 0;
    u64 ticks = 0, resumes = 0, wakes = 0, syscalls = 0;
    u64 allocs = 0, frees = 0, grows = 0;
    u64 in_use = 0, reserved = 0;
    u64 ready = 0, on_timer = 0, on_host = 0, on_park = 0;
};

// Here rather than in text.h, which every binary links, because this is its one
// caller: a counter outgrows u32.
Option<u64> parse_u64(Str s)
{
    if (s.empty())
        return {};
    u64 v = 0;
    for (usize i = 0; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9')
            return {};
        u64 d = u64(s[i] - '0');
        if (v > (~u64(0) - d) / 10)
            return {};
        v = v * 10 + d;
    }
    return v;
}

// The value on a `name<padding> value` line, which is how /proc/stat and
// /proc/host both read. The rest of the line, so a value may hold a space.
Str value_of(Str text, Str want)
{
    Str rest = text, line;
    while (next_line(rest, line)) {
        Str probe = line;
        if (next_field(probe) != want)
            continue;
        usize at = 0;
        while (at < probe.size() && probe[at] == ' ')
            at++;
        return probe.substr(at);
    }
    return {};
}

u64 number_of(Str text, Str want)
{
    Option<u64> v = parse_u64(value_of(text, want));
    return v ? v.value() : 0;
}

void fill(Sample &s, Str text)
{
    s.now      = number_of(text, "now");
    s.ticks    = number_of(text, "ticks");
    s.resumes  = number_of(text, "resumes");
    s.wakes    = number_of(text, "wakes");
    s.syscalls = number_of(text, "syscalls");
    s.allocs   = number_of(text, "allocs");
    s.frees    = number_of(text, "frees");
    s.grows    = number_of(text, "grows");
    s.in_use   = number_of(text, "in_use");
    s.reserved = number_of(text, "reserved");
    s.ready    = number_of(text, "ready");
    s.on_timer = number_of(text, "on_timer");
    s.on_host  = number_of(text, "on_host");
    s.on_park  = number_of(text, "on_park");
}

// Per second, rounded rather than truncated, as BSD's rate() is. The caller
// guarantees a divisor.
u64 rate(u64 delta, u64 ms)
{
    return (delta * 1000 + ms / 2) / ms;
}

u64 grew(u64 now, u64 before)
{
    return now >= before ? now - before : 0; // a reset is the only way backwards
}

// A group heading centred in the width its fields occupy, so the rules cannot
// drift from the columns under them.
template <usize N>
void rule(Buf<N> &out, Str label, usize w)
{
    usize pad  = label.size() < w ? w - label.size() : 0;
    usize left = pad / 2;
    for (usize i = 0; i < left; i++)
        out.put('-');
    out.put(label);
    for (usize i = left; i < pad; i++)
        out.put('-');
}

template <usize N>
void gap(Buf<N> &out)
{
    for (usize i = 0; i < GAP; i++)
        out.put(' ');
}

// Both heading lines, built rather than written, so this is no coroutine and
// costs no frame.
void put_header(Buf<192> &out)
{
    rule(out, "procs", W_PROC * 4);
    gap(out);
    rule(out, "memory", W_MEM * 2);
    gap(out);
    rule(out, "alloc", W_AL + W_FR + W_GR);
    gap(out);
    rule(out, "faults", W_RATE * 3);
    gap(out);
    rule(out, "loop", W_TICK);
    out.put('\n');

    out.put_right("r", W_PROC).put_right("t", W_PROC);
    out.put_right("h", W_PROC).put_right("p", W_PROC);
    gap(out);
    out.put_right("use", W_MEM).put_right("fre", W_MEM);
    gap(out);
    out.put_right("al", W_AL).put_right("fr", W_FR).put_right("gr", W_GR);
    gap(out);
    out.put_right("in", W_RATE).put_right("sy", W_RATE).put_right("cs", W_RATE);
    gap(out);
    out.put_right("tk", W_TICK);
    out.put('\n');
}

// The gauges as they stand, the counters as rates over `ms`. Nothing to divide
// by is not a rate of nought, so it prints `-`, the way `df` prints a capacity.
void put_row(Buf<128> &out, const Sample &s, const Sample &was, u64 ms)
{
    out.put_right(s.ready, W_PROC).put_right(s.on_timer, W_PROC);
    out.put_right(s.on_host, W_PROC).put_right(s.on_park, W_PROC);
    gap(out);
    out.put_right(s.in_use / 1024, W_MEM);
    out.put_right(grew(s.reserved, s.in_use) / 1024, W_MEM);
    gap(out);

    const u64 deltas[]   = { grew(s.allocs, was.allocs),     grew(s.frees, was.frees),
                             grew(s.grows, was.grows),       grew(s.wakes, was.wakes),
                             grew(s.syscalls, was.syscalls), grew(s.resumes, was.resumes),
                             grew(s.ticks, was.ticks) };
    const usize widths[] = { W_AL, W_FR, W_GR, W_RATE, W_RATE, W_RATE, W_TICK };
    for (usize i = 0; i < sizeof widths / sizeof widths[0]; i++) {
        if (i == 3 || i == 6)
            gap(out);
        if (!ms)
            out.put_right("-", widths[i]);
        else
            out.put_right(rate(deltas[i], ms), widths[i]);
    }
    out.put('\n');
}

// What each counter is, for -s. A name the table does not carry prints under
// itself, so a counter added to the kernel needs no edit here.
Str meaning_of(Str name)
{
    struct Meaning {
        Str name, text;
    };
    constexpr Meaning TABLE[] = {
        { "now", "milliseconds since boot" },
        { "ticks", "turns of the event loop" },
        { "resumes", "coroutine resumptions" },
        { "timers", "timer expiries" },
        { "wakes", "answers delivered by the host" },
        { "unparks", "wakeups from a channel" },
        { "misses", "wakes with nothing waiting" },
        { "spawns", "tasks created" },
        { "syscalls", "syscalls parked and answered" },
        { "sysfast", "syscalls answered in the import" },
        { "steps", "steps issued to a worker" },
        { "in_use", "heap bytes in use" },
        { "reserved", "heap bytes reserved" },
        { "allocs", "heap allocations" },
        { "frees", "heap frees" },
        { "frames", "coroutine frames allocated" },
        { "grows", "growths of linear memory" },
        { "fails", "allocations refused" },
        { "tasks", "tasks running now" },
        { "ready", "of them runnable" },
        { "on_timer", "of them waiting on a timer" },
        { "on_host", "of them waiting on the host" },
        { "on_park", "of them parked on a channel" },
    };
    for (const Meaning &m : TABLE)
        if (m.name == name)
            return m.text;
    return name;
}

// Every counter, one per line, the way BSD's -s prints the sum structure.
Task<Result<void>> summary(Str text)
{
    Str rest = text, line;
    while (next_line(rest, line)) {
        Str name = next_field(line);
        if (name.empty())
            continue;
        Buf<128> out;
        out.put_right(value_of(text, name), 9).put(' ').put(meaning_of(name)).put('\n');
        CO_TRY_VOID(co_await write_all(SYS_STDOUT, out.str()));
    }
    co_return {};
}

// The window's rows, for the header repeat. /proc/host says `screen <cols>x<rows>`.
usize screen_rows(Str host)
{
    Str screen = value_of(host, "screen");
    usize at   = 0;
    while (at < screen.size() && screen[at] != 'x')
        at++;
    if (at == screen.size())
        return FALLBACK_ROWS;
    Option<u32> rows = parse_u32(screen.substr(at + 1));
    return rows && rows.value() > 2 ? usize(rows.value()) : FALLBACK_ROWS;
}

constexpr Str USAGE = "usage: vmstat [-s] [-w <secs>] [-c <count>] [<secs> [<count>]]\n";

} // namespace

Task<i32> proc_main(Args args)
{
    bool sum = false, bad = false;
    u32 interval = 0, reps = 0; // reps 0 is "until ^C"

    usize i = 1;
    for (; i < args.size() && !bad; i++) {
        Str a = args[i];
        if (a.size() < 2 || a[0] != '-')
            break;
        if (a == "-s") {
            sum = true;
            continue;
        }
        // -w and -c take the next word: nothing in the tree accepts -w5 or -sc.
        if ((a != "-w" && a != "-c") || i + 1 >= args.size()) {
            bad = true;
            break;
        }
        Option<u32> v = parse_u32(args[i + 1]);
        if (!v) {
            bad = true;
            break;
        }
        if (a == "-w")
            interval = v.value();
        else
            reps = v.value();
        i++;
    }

    // BSD's backward-compatible spelling, read after the flags and therefore
    // winning over them.
    if (!bad && i < args.size()) {
        Option<u32> v = parse_u32(args[i++]);
        if (!v)
            bad = true;
        else
            interval = v.value();
        if (!bad && i < args.size()) {
            Option<u32> n = parse_u32(args[i++]);
            if (!n)
                bad = true;
            else
                reps = n.value();
        }
    }
    if (i < args.size())
        bad = true;

    // -s is the whole output when it is asked for, so an interval would have
    // nothing to pace. BSD ignores one silently; saying so costs nothing.
    if (sum && (interval || reps))
        bad = true;

    if (interval > MAX_SECS)
        bad = true;

    if (bad) {
        co_await write_all(SYS_STDERR, USAGE);
        co_return 2;
    }

    // A count without an interval paces itself, as BSD's does.
    if (reps && !interval)
        interval = DEFAULT_SECS;

    // The wait is what `sleep_for` takes, and the fallback divisor when two reads
    // land inside one millisecond of the kernel's clock.
    const u32 wait_ms = interval * 1000;

    Result<String> first = Err(Error::NoMemory);
    if (Task<Result<String>> t = read_file("/proc/stat"))
        first = co_await t;
    if (first.is_err()) {
        if (first.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("vmstat", "/proc/stat", first.error()))
            co_await e;
        co_return 1;
    }

    if (sum) {
        Result<void> r = Err(Error::NoMemory);
        if (Task<Result<void>> t = summary(first.value().str()))
            r = co_await t;
        if (r.is_err())
            co_return r.error() == Error::Cancelled ? 130 : 1;
        co_return 0;
    }

    // The rows the window holds, asked once. A resize mid-run leaves the count
    // stale, which is BSD's behaviour too: it needs a signal to notice.
    usize rows = FALLBACK_ROWS;
    if (Task<Result<String>> t = read_file("/proc/host")) {
        Result<String> host = co_await t;
        if (host.is_ok())
            rows = screen_rows(host.value().str());
    }

    Sample now, was;
    fill(now, first.value().str());

    // The first row is since boot, so the divisor is the uptime and the counters
    // are their own deltas — exactly BSD, whose rate() divides by getuptime().
    u64 ms = now.now ? now.now : 1;

    // Rows before the next heading, which is two of them: BSD's hdrcnt.
    usize left = 0;
    for (u32 printed = 0;; printed++) {
        if (!left) {
            Buf<192> head;
            put_header(head);
            if ((co_await write_all(SYS_STDOUT, head.str())).is_err())
                co_return 1;
            left = rows - 2;
        }

        Buf<128> line;
        put_row(line, now, was, ms);
        if ((co_await write_all(SYS_STDOUT, line.str())).is_err())
            co_return 1;
        left--;

        if (reps && printed + 1 >= reps)
            co_return 0;
        if (!interval)
            co_return 0;

        Result<void> slept = Err(Error::NoMemory);
        if (Task<Result<void>> t = sleep_for(wait_ms))
            slept = co_await t;
        if (slept.is_err())
            co_return 130; // ^C, which is the only way out of an endless run

        was                 = now;
        Result<String> next = Err(Error::NoMemory);
        if (Task<Result<String>> t = read_file("/proc/stat"))
            next = co_await t;
        if (next.is_err()) {
            if (next.error() == Error::Cancelled)
                co_return 130;
            if (Task<void> e = errln("vmstat", "/proc/stat", next.error()))
                co_await e;
            co_return 1;
        }
        fill(now, next.value().str());

        // The file's own clock, not how long the sleep took.
        ms = grew(now.now, was.now);
        if (!ms)
            ms = wait_ms;
    }
}

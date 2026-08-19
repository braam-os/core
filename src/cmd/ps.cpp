#include "kernel/fmt.h"
#include "kernel/text.h"
#include "proc/io.h"

// The tasks the scheduler is running, from /proc/tasks — one open, so every row
// describes the same moment. A worker is exactly a process, and a process is
// exactly a row with a cwd: a task without one is a coroutine in the kernel, and
// answers for nothing in the four columns after STAT.
//
// MEM is what the instance has committed, measured on the host — the one memory
// figure a browser gives up, since a wasm Memory says its own size even though
// nothing will say a worker's. It rides back on every step, so it is current as
// of the last one. The cap it may grow to is /proc/<pid>. There is still no CPU
// column: nothing meters one.

namespace {

// A row inside eighty columns, the cwd taking whatever is left. NAME holds the
// longest a syscall server carries — a binary's path, /bin/timeout — and one
// space.
constexpr usize W_PID = 5, W_PPID = 5, W_NAME = 13, W_STAT = 5;
constexpr usize W_WAIT = 6, W_CALLS = 6, W_FDS = 4, W_MEM = 6, W_AGE = 8;

// R ready, S waiting, C cancelled — then what the task holds, which /proc has
// already packed into a word of its own.
void put_stat(Buf<16> &b, Str state, Str flags)
{
    b.put(state == "cancelled" ? 'C' : state == "waiting" ? 'S' : 'R');
    if (flags != "-")
        b.put(flags);
}

// 1.2M, 448K, 16M — one decimal below ten, the way df prints a size.
void put_mem(Buf<128> &out, Str bytes, usize w)
{
    Option<u32> v = parse_u32(bytes);
    if (!v || !v.value()) {
        out.put_right("-", w);
        return;
    }

    constexpr Str UNIT[] = { "B", "K", "M", "G" };
    u32 whole = v.value(), rem = 0, u = 0;
    while (whole >= 1024 && u + 1 < sizeof(UNIT) / sizeof(UNIT[0])) {
        rem = whole % 1024;
        whole /= 1024;
        u++;
    }

    Buf<16> t;
    t.put(whole);
    if (u && whole < 10) {
        u32 tenth = (rem * 10) / 1024;
        if (tenth)
            t.put('.').put(tenth);
    }
    t.put(UNIT[u]);
    out.put_right(t.str(), w);
}

// m:ss, and h:mm:ss once it has been up an hour. Milliseconds are what /proc
// counts in and nothing here wants them.
void put_age(Buf<128> &out, Str ms, usize w)
{
    Option<u32> v = parse_u32(ms);
    if (!v) {
        out.put_right("-", w);
        return;
    }

    u32 secs = v.value() / 1000;
    Buf<16> t;
    if (secs >= 3600)
        t.put(secs / 3600).put(':').put((secs / 60) % 60 < 10 ? "0" : "").put((secs / 60) % 60);
    else
        t.put(secs / 60);
    t.put(':').put(secs % 60 < 10 ? "0" : "").put(secs % 60);
    out.put_right(t.str(), w);
}

// A count only a process has, so a kernel task shows nothing rather than nought.
void put_count(Buf<128> &out, Str value, bool proc, usize w)
{
    out.put_right(proc ? value : Str("-"), w);
}

// Cut to the column rather than written whole: a name is argv[0] or a binary's
// path, and one wide enough would push STAT out of its own column.
void put_name(Buf<128> &out, Str name, usize w)
{
    out.put_left(name.size() < w ? name : name.substr(0, w - 1), w);
}

// What is left of the line, padding gone: the last field, since a path may hold
// a space and next_field would stop at it.
Str last_field(Str line)
{
    usize at = 0;
    while (at < line.size() && line[at] == ' ')
        at++;
    return line.substr(at);
}

} // namespace

Task<i32> proc_main(Args args)
{
    if (args.size() > 1) {
        co_await write_all(SYS_STDERR, "usage: ps\n");
        co_return 2;
    }

    Result<String> r = Err(Error::NoMemory);
    if (Task<Result<String>> t = read_file("/proc/tasks"))
        r = co_await t;
    if (r.is_err()) {
        if (r.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("ps", "/proc/tasks", r.error()))
            co_await e;
        co_return 1;
    }

    Buf<128> head;
    head.put_right("PID", W_PID).put_right("PPID", W_PPID).put(' ');
    head.put_left("NAME", W_NAME).put_left("STAT", W_STAT);
    head.put_left("WAIT", W_WAIT);
    head.put_right("CALLS", W_CALLS).put_right("FDS", W_FDS);
    head.put_right("MEM", W_MEM).put_right("ELAPSED", W_AGE).put("  CWD\n");
    if ((co_await write_all(SYS_STDOUT, head.str())).is_err())
        co_return 1;

    // The fields /proc/tasks puts in order. The cwd is the rest of the line,
    // which is why it is last there: a path may hold a space.
    Str rest = r.value().str(), line;
    while (next_line(rest, line)) {
        Str pid   = next_field(line);
        Str name  = next_field(line);
        Str state = next_field(line);
        Str wait  = next_field(line);
        Str flags = next_field(line);
        Str ppid  = next_field(line);
        Str calls = next_field(line);
        Str fds   = next_field(line);
        Str mem   = next_field(line);
        next_field(line); // the cap, which is uniform: /proc/<pid> has it
        Str age = next_field(line);
        Str cwd = last_field(line);
        if (pid.empty())
            continue;

        bool proc = cwd != "-";

        Buf<16> stat;
        put_stat(stat, state, flags);

        Buf<128> out;
        out.put_right(pid, W_PID);
        put_count(out, ppid, ppid != "0", W_PPID);
        out.put(' ');
        put_name(out, name, W_NAME);
        out.put_left(stat.str(), W_STAT);
        out.put_left(wait, W_WAIT);
        put_count(out, calls, proc, W_CALLS);
        put_count(out, fds, proc, W_FDS);
        put_mem(out, mem, W_MEM);
        put_age(out, age, W_AGE);
        out.put("  ").put(cwd).put('\n');
        if ((co_await write_all(SYS_STDOUT, out.str())).is_err())
            co_return 1;
    }
    co_return 0;
}

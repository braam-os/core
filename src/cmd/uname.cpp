#include "kernel/fmt.h"
#include "proc/io.h"

// The kernel publishes what the system is and what it runs on as /proc/host,
// so this reformats that file rather than asking for an operation of its own —
// mount.cpp's arrangement, for mount.cpp's reason.
//
// -a prints the whole block rather than packing it onto one line the way POSIX
// uname does. There is no POSIX contract here, and the block is what is worth
// reading: a browser discloses more about itself than four fields hold.

namespace {

// The value of one `key value` line, or empty when the file has no such key.
// A blank line separates the fields the boot banner showed from the rest; it
// has no key, so it is skipped like any other line that does not match.
Str field(Str text, Str want)
{
    Str rest = text, line;
    while (next_line(rest, line)) {
        Str key = next_field(line);
        if (key != want)
            continue;

        // next_field stops at the run of spaces that pads the column and leaves
        // it on the line. Only that goes: a value may hold a space of its own.
        usize at = 0;
        while (at < line.size() && line[at] == ' ')
            at++;
        return line.substr(at);
    }
    return Str();
}

Task<i32> one(Str text, Str key)
{
    Str value = field(text, key);
    if (value.empty())
        co_return 1;

    Buf<96> b;
    b.put(value).put('\n');
    co_return (co_await write_all(SYS_STDOUT, b.str())).is_err() ? 1 : 0;
}

} // namespace

Task<i32> proc_main(Args args)
{
    Str flag;
    if (args.size() > 2) {
        co_await write_all(SYS_STDERR, "usage: uname [-a|-s|-r|-m]\n");
        co_return 2;
    }
    if (args.size() == 2) {
        flag = args[1];
        if (flag != "-a" && flag != "-s" && flag != "-r" && flag != "-m") {
            co_await write_all(SYS_STDERR, "usage: uname [-a|-s|-r|-m]\n");
            co_return 2;
        }
    }

    Result<String> r = Err(Error::NoMemory);
    if (Task<Result<String>> t = read_file("/proc/host"))
        r = co_await t;
    if (r.is_err()) {
        if (r.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("uname", "/proc/host", r.error()))
            co_await e;
        co_return 1;
    }
    Str text = r.value().str();

    // The default is the system name, as uname's has always been.
    if (flag.empty() || flag == "-s")
        co_return co_await one(text, "system");
    if (flag == "-r")
        co_return co_await one(text, "release");
    if (flag == "-m")
        co_return co_await one(text, "machine");

    // -a: every field the file carries, in the order it carries them, with the
    // blank line dropped — it is a marker for the banner, not something to show.
    Str rest = text, line;
    while (next_line(rest, line)) {
        if (line.empty())
            continue;

        Buf<160> b;
        b.put(line).put('\n');
        if ((co_await write_all(SYS_STDOUT, b.str())).is_err())
            co_return 1;
    }
    co_return 0;
}

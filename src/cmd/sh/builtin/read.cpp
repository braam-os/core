#include "cmd/sh/var.h"
#include "decl.h"
#include "kernel/alloc.h"
#include "kernel/string.h"

namespace {

// What the last read took past its newline. Sys::Read carries no length, so a
// chunk is whatever the writer wrote and a read of one line keeps the rest of
// it. Kept per descriptor and dropped when the next read is on another one:
// two streams cannot share a pushback. A pointer, so the global stays
// trivially destructible (CLAUDE.md).
String *g_rest;
u32 g_rest_fd;

bool is_ifs(char c, Str ifs)
{
    return ifs.find(c) != Str::npos;
}

// One field, advancing `rest` past it.
Str take_field(Str &rest, Str ifs)
{
    usize i = 0;
    while (i < rest.size() && is_ifs(rest[i], ifs))
        i++;
    usize from = i;
    while (i < rest.size() && !is_ifs(rest[i], ifs))
        i++;
    Str f = rest.substr(from, i - from);
    rest  = rest.substr(i);
    return f;
}

// What the last name takes: the rest, separators and all, trimmed at both ends.
Str take_rest(Str rest, Str ifs)
{
    usize a = 0, b = rest.size();
    while (a < b && is_ifs(rest[a], ifs))
        a++;
    while (b > a && is_ifs(rest[b - 1], ifs))
        b--;
    return rest.substr(a, b - a);
}

// One line off `fd`, without its newline. Err(Closed) at end of input.
Task<Result<String>> read_line(u32 fd)
{
    String buf;
    if (g_rest && g_rest_fd == fd && !buf.assign(g_rest->str()))
        co_return Err(Error::NoMemory);
    heap_delete(g_rest);
    g_rest = nullptr;

    for (;;) {
        usize nl = buf.str().find('\n');
        if (nl != Str::npos) {
            String line, rest;
            if (!line.assign(buf.str().substr(0, nl)) || !rest.assign(buf.str().substr(nl + 1)))
                co_return Err(Error::NoMemory);
            if (!rest.empty()) {
                g_rest = heap_new<String>(move(rest));
                if (!g_rest)
                    co_return Err(Error::NoMemory);
                g_rest_fd = fd;
            }
            co_return move(line);
        }

        Task<Result<String>> t = read_chunk(fd);
        Result<String> r       = t ? co_await t : Err(Error::NoMemory);
        if (r.is_err()) {
            // A last line with no newline is a line.
            if (r.error() == Error::Closed && !buf.empty())
                co_return move(buf);
            co_return Err(r.error());
        }
        if (!buf.append(r.value().str()))
            co_return Err(Error::NoMemory);
    }
}

} // namespace

// A builtin because the variables it fills are this process's own. At an
// interactive prompt it needs nothing special: run_line has handed the
// keyboard back before any builtin runs, so the console pump is cooking lines
// into this shell's stdin and doing the echo.
Task<i32> builtin_read(Args args, ShIo io)
{
    if (args.size() < 2) {
        co_await write_all(io.err, "usage: read <name>...\n");
        co_return 2;
    }

    Task<Result<String>> t = read_line(io.in);
    Result<String> r       = t ? co_await t : Err(Error::NoMemory);
    if (r.is_err()) {
        if (r.error() == Error::Cancelled)
            co_return 130;
        co_return 1; // end of input, which is what ends a `while read`
    }

    // Copied: var_get hands back a view and the table moves under it.
    String ifs;
    Str want;
    if (!var_get("IFS", want))
        want = " \t\n";
    if (!ifs.assign(want))
        co_return 1;

    Str rest   = r.value().str();
    i32 status = 0;
    for (usize i = 1; i < args.size(); i++) {
        bool last = i + 1 == args.size();
        Str value = last ? take_rest(rest, ifs.str()) : take_field(rest, ifs.str());
        if (!var_set(args[i], value)) {
            if (Task<void> e = errln("read", args[i], Error::Perm))
                co_await e;
            status = 1;
        }
    }
    co_return status;
}

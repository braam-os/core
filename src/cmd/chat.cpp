#include "proc/io.h"

// Sending and receiving are two waits at once, which is the whole reason a
// process may have more than one task: the receiver is parked on the socket
// while the root task is parked on what is being typed.
//
// As an applet this had to be a scheduler job outside the program, writing
// straight to the screen because the pipe it would have written to belonged to
// a job that might already be gone — so `chat > log` captured nothing. Inside a
// process the descriptors are the process's own and live exactly as long as it
// does, so the receiver writes to stdout like anything else, and redirection
// works.

namespace {

Task<i32> receive(i32 fd)
{
    for (;;) {
        Result<String> msg = Err(Error::NoMemory);
        if (Task<Result<String>> t = read_chunk(u32(fd)))
            msg = co_await t;
        if (msg.is_err()) {
            if (msg.error() != Error::Closed)
                co_return 1;
            co_await write_all(SYS_STDOUT, "chat: the peer closed the connection\n");
            co_return 0;
        }

        String line;
        if (!line.append(msg.value().str()) || !line.push('\n'))
            co_return 1;
        if ((co_await write_all(SYS_STDOUT, line.str())).is_err())
            co_return 1;
    }
}

} // namespace

Task<i32> proc_main(Args args)
{
    if (args.size() < 2 || args.size() > 3) {
        co_await write_all(SYS_STDERR, "usage: chat <url> [<nick>]\n");
        co_return 2;
    }

    Result<i32> sock = Err(Error::NoMemory);
    if (Task<Result<i32>> t = ws_connect(args[1]))
        sock = co_await t;
    if (sock.is_err()) {
        if (sock.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("chat", args[1], sock.error()))
            co_await e;
        co_return 1;
    }

    if (!proc_spawn(receive(sock.value()))) {
        co_await write_all(SYS_STDERR, "chat: no room for a receiver\n");
        co_return 1;
    }

    Str nick = args.size() == 3 ? args[2] : Str();
    Input in(Args{}, SYS_STDIN);
    LineReader lines(in);
    String line;

    for (;;) {
        Result<bool> r = co_await lines.next(line);
        if (r.is_err())
            co_return r.error() == Error::Cancelled ? 130 : 1;
        if (!r.value()) {
            // End of input closes the connection rather than leaving it to the
            // process's teardown — and the receiver is parked on that socket,
            // so this closes a descriptor another task is inside a read of.
            if (Task<void> c = close_fd(u32(sock.value())))
                co_await c;
            co_return 0;
        }

        String out;
        if (!nick.empty() && (!out.append(nick) || !out.append(": ")))
            co_return 1;
        if (!out.append(line.str()))
            co_return 1;

        if ((co_await write_all(u32(sock.value()), out.str())).is_err()) {
            co_await write_all(SYS_STDERR, "chat: the connection is gone\n");
            co_return 1;
        }
    }
}

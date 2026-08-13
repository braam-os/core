#include "kernel/alloc.h"
#include "kernel/sched.h"
#include "kernel/screen.h"
#include "kernel/traits.h"
#include "svc/net.h"
#include "user/io.h"
#include "user/prog.h"

namespace {

// Sending and receiving are two waits, and CancelState::waiting is one slot, so
// the receiver has to be a scheduler job of its own — §3.6's structured
// concurrency put back by hand, as the pipeline does.
struct Session {
    u32 refs = 1;
    WebSocket ws;
};

void session_release(Session *s)
{
    if (--s->refs == 0)
        heap_delete(s);
}

// The receiver outlives its parent by a tick or two, so it touches nothing the
// parent owns: the session is refcounted, and incoming lines go straight to the
// screen rather than through a Stdio handle whose pipe may already be gone.
// `chat > log` therefore does not capture what arrives, which is what an
// interactive program means until M7 gives one a pane of its own.
Task<i32> receive(Session *s)
{
    struct End {
        ~End() { session_release(s); }

        Session *s;
    } end{ s };

    for (;;) {
        Result<String> msg = Err(Error::NoMemory);
        if (Task<Result<String>> t = ws_recv(s->ws))
            msg = co_await t;
        if (msg.is_err()) {
            if (msg.error() == Error::Closed) {
                screen_write("chat: the peer closed the connection");
                screen_newline();
            }
            co_return msg.error() == Error::Closed ? 0 : 1;
        }
        screen_write(msg.value().str());
        screen_newline();
    }
}

} // namespace

BRAAM_PROGRAM(prog_chat, "chat", "<url> [<nick>] — talk over a WebSocket")
{
    if (args.size() < 2 || args.size() > 3) {
        co_await io.err.write("usage: chat <url> [<nick>]\n");
        co_return 2;
    }

    Result<WebSocket> got = Err(Error::NoMemory);
    if (Task<Result<WebSocket>> t = ws_open(args[1]))
        got = co_await t;
    if (got.is_err()) {
        if (got.error() == Error::Cancelled)
            co_return 130;
        co_await io.err.write("chat: ");
        co_await io.err.write(args[1]);
        co_await io.err.write(": ");
        co_await io.err.write(error_name(got.error()));
        co_await io.err.write("\n");
        co_return 1;
    }

    Session *s = heap_new<Session>();
    if (!s)
        co_return 1;
    s->ws = move(got.value());

    s->refs++;
    u32 pid = sched_spawn(receive(s), "chat-recv");
    if (!pid)
        s->refs--;

    // A destructor, so that ^C takes the receiver with it: the parent's frame
    // is destroyed while suspended, and this is the only code that still runs.
    struct Stop {
        ~Stop()
        {
            if (pid)
                sched_cancel(pid);
            session_release(s);
        }

        Session *s;
        u32 pid;
    } stop{ s, pid };

    Str nick = args.size() == 3 ? args[2] : Str();
    LineReader in(io.in);
    String line;
    for (;;) {
        Result<bool> r = co_await in.next(line);
        if (r.is_err())
            co_return r.error() == Error::Cancelled ? 130 : 1;
        if (!r.value())
            co_return 0;

        String out;
        if (!nick.empty() && (!out.append(nick) || !out.append(": ")))
            co_return 1;
        if (!out.append(line.str()))
            co_return 1;

        Result<void> sent = Err(Error::NoMemory);
        if (Task<Result<void>> t = ws_send(s->ws, out.str()))
            sent = co_await t;
        if (sent.is_err()) {
            if (sent.error() == Error::Cancelled)
                co_return 130;
            co_await io.err.write("chat: the connection is gone\n");
            co_return 1;
        }
    }
}

#include "kernel/fmt.h"
#include "proc/io.h"

// A relative URL resolves against the page, so `curl /index.html` works with no
// network at all. A cross-origin one needs CORS, which is the wall a user meets
// first and the reason the diagnostic mentions it.
Task<i32> proc_main(Args args)
{
    bool show_head = false;
    String spec;
    String method;
    String headers;
    String data;

    usize i = 1;
    for (; i < args.size(); i++) {
        Str a = args[i];
        if (a == "-i") {
            show_head = true;
        } else if (a == "-X" && i + 1 < args.size()) {
            if (!method.assign(args[++i]))
                co_return 1;
        } else if (a == "-H" && i + 1 < args.size()) {
            if (!headers.append(args[++i]) || !headers.push('\n'))
                co_return 1;
        } else if (a == "-d" && i + 1 < args.size()) {
            if (!data.assign(args[++i]))
                co_return 1;
        } else {
            break;
        }
    }

    if (i + 1 != args.size()) {
        co_await write_all(SYS_STDERR,
                           "usage: curl [-i] [-X <method>] [-H <header>] [-d <data>] <url>\n");
        co_return 2;
    }

    if (method.empty() && !method.assign(data.empty() ? Str("GET") : Str("POST")))
        co_return 1;
    if (!spec.append(method.str()) || !spec.push('\n') || !spec.append(headers.str()) ||
        !spec.push('\n') || !spec.append(data.str()))
        co_return 1;

    Result<Fetched> got = Err(Error::NoMemory);
    if (Task<Result<Fetched>> t = fetch_url(args[i], spec.str()))
        got = co_await t;
    if (got.is_err()) {
        if (got.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("curl", args[i], got.error()))
            co_await e;
        if (got.error() == Error::Io)
            co_await write_all(SYS_STDERR, "curl: a cross-origin URL needs CORS\n");
        co_return 1;
    }

    const Fetched &res = got.value();
    if (show_head) {
        Buf<32> line;
        line.put("HTTP ").put(res.status).put("\n");
        if ((co_await write_all(SYS_STDOUT, line.str())).is_err())
            co_return 1;
        if ((co_await write_all(SYS_STDOUT, res.headers.str())).is_err())
            co_return 1;
        if ((co_await write_all(SYS_STDOUT, "\n")).is_err())
            co_return 1;
    }

    i32 status = 0;
    for (;;) {
        Result<String> chunk = Err(Error::NoMemory);
        if (Task<Result<String>> t = read_chunk(u32(res.body)))
            chunk = co_await t;
        if (chunk.is_err()) {
            if (chunk.error() == Error::Closed)
                break;
            status = chunk.error() == Error::Cancelled ? 130 : 1;
            break;
        }
        if ((co_await write_all(SYS_STDOUT, chunk.value().str())).is_err()) {
            status = 1;
            break;
        }
    }

    if (Task<void> c = close_fd(u32(res.body)))
        co_await c;

    // 404 is a fetch that worked and an answer the user did not want.
    if (status)
        co_return status;
    co_return res.status >= 400 ? 1 : 0;
}

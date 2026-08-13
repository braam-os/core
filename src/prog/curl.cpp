#include "kernel/fmt.h"
#include "svc/net.h"
#include "user/io.h"
#include "user/prog.h"

// A relative URL resolves against the page, so `curl /index.html` works with no
// network at all. A cross-origin one needs CORS, which is the wall a user meets
// first and the reason the diagnostic mentions it.
BRAAM_PROGRAM(prog_curl, "curl", "[-i] [-X <method>] [-H <header>] [-d <data>] <url> — fetch a URL")
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
        co_await io.err.write("usage: curl [-i] [-X <method>] [-H <header>] [-d <data>] <url>\n");
        co_return 2;
    }

    if (method.empty() && !method.assign(data.empty() ? "GET" : "POST"))
        co_return 1;
    if (!spec.append(method.str()) || !spec.push('\n') || !spec.append(headers.str()) ||
        !spec.push('\n') || !spec.append(data.str()))
        co_return 1;

    Result<HttpResponse> got = Err(Error::NoMemory);
    if (Task<Result<HttpResponse>> t = http_fetch(args[i], spec.str()))
        got = co_await t;
    if (got.is_err()) {
        if (got.error() == Error::Cancelled)
            co_return 130;
        co_await io.err.write("curl: ");
        co_await io.err.write(args[i]);
        co_await io.err.write(": ");
        co_await io.err.write(error_name(got.error()));
        if (got.error() == Error::Io)
            co_await io.err.write(" (a cross-origin URL needs CORS)");
        co_await io.err.write("\n");
        co_return 1;
    }

    const HttpResponse &res = got.value();
    if (show_head) {
        Buf<32> line;
        line.put("HTTP ").put(res.status).put("\n");
        if ((co_await write_all(io.out, line.str())).is_err())
            co_return 1;
        if ((co_await write_all(io.out, res.headers.str())).is_err())
            co_return 1;
        if ((co_await write_all(io.out, "\n")).is_err())
            co_return 1;
    }

    for (;;) {
        Result<String> chunk = Err(Error::NoMemory);
        if (Task<Result<String>> t = http_read(res))
            chunk = co_await t;
        if (chunk.is_err())
            co_return chunk.error() == Error::Cancelled ? 130 : 1;
        if (chunk.value().empty())
            break;
        if ((co_await write_all(io.out, chunk.value().str())).is_err())
            co_return 1;
    }

    // 404 is a fetch that worked and an answer the user did not want.
    co_return res.status >= 400 ? 1 : 0;
}

#include "net.h"

#include "kernel/traits.h"

Task<Result<HttpResponse>> http_fetch(Str url, Str spec)
{
    SvcCall c(SvcOp::Fetch, url, 0);
    if (!c.ok() || !c.put(spec) || !c.reserve_ref())
        co_return Err(Error::NoMemory);

    // The headers are sized twice, like a directory listing: a response with
    // ordinary headers costs one round trip and a chatty one costs two.
    if (!c.reserve(512))
        co_return Err(Error::NoMemory);

    for (int attempt = 0; attempt < 2; attempt++) {
        CO_TRY_VOID(co_await c);
        HostReq &r = c.req();
        if (r.h.buf_len == 0 && r.h.result_hi != 0) {
            if (!c.reserve(r.h.result_hi))
                co_return Err(Error::NoMemory);
            r.done = false;
            continue;
        }
        HttpResponse res;
        res.status = r.h.result_lo;
        if (r.h.buf_len && !res.headers.append(Str(r.buf.data(), r.h.buf_len)))
            co_return Err(Error::NoMemory);
        res.body = JsHandle(c.take_ref());
        co_return move(res);
    }
    co_return Err(Error::Io);
}

Task<Result<String>> http_read(const HttpResponse &res)
{
    SvcCall c(SvcOp::Read);
    if (!c.ok())
        co_return Err(Error::NoMemory);
    c.set_ref(res.body.slot());

    Task<Result<String>> t = svc_chunk(c);
    if (!t)
        co_return Err(Error::NoMemory);
    co_return co_await t;
}

Task<Result<WebSocket>> ws_open(Str url)
{
    SvcCall c(SvcOp::WsOpen, url, 0);
    if (!c.ok() || !c.reserve_ref())
        co_return Err(Error::NoMemory);
    CO_TRY_VOID(co_await c);

    WebSocket ws;
    ws.sock = JsHandle(c.take_ref());
    co_return move(ws);
}

Task<Result<void>> ws_send(const WebSocket &ws, Str msg)
{
    SvcCall c(SvcOp::WsSend, "", 0);
    if (!c.ok() || !c.put(msg))
        co_return Err(Error::NoMemory);
    c.set_ref(ws.sock.slot());
    CO_TRY_VOID(co_await c);
    co_return {};
}

Task<Result<String>> ws_recv(const WebSocket &ws)
{
    SvcCall c(SvcOp::WsRecv);
    if (!c.ok())
        co_return Err(Error::NoMemory);
    c.set_ref(ws.sock.slot());

    Task<Result<String>> t = svc_blob(c);
    if (!t)
        co_return Err(Error::NoMemory);
    String msg = CO_TRY(co_await t);

    // The socket closing is a reply like any other, so that a reader parked on
    // it unwinds instead of waiting for a message that will never come.
    if (c.req().h.flags & 1)
        co_return Err(Error::Closed);
    co_return move(msg);
}

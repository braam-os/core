// fetch and WebSocket, over the service ABI. Both hand back a JS object the
// kernel then holds in an externref slot (Concept.md §3.7); dropping the
// handle is what closes the socket or cancels the body.
#pragma once

#include "svc.h"

// A response whose headers have arrived. The body is read afterwards, a chunk
// at a time, so a large one never has to fit in a buffer at once.
struct HttpResponse {
    u32 status = 0;
    String headers; // "name: value\n" lines, as the host joined them
    JsHandle body;
};

// `spec` is the method, then a blank line, then any request headers, then a
// blank line, then the body — the shape web/svc.js parses.
Task<Result<HttpResponse>> http_fetch(Str url, Str spec);

// A chunk of the body, empty at the end.
Task<Result<String>> http_read(const HttpResponse &res);

struct WebSocket {
    JsHandle sock;
};

Task<Result<WebSocket>> ws_open(Str url);
Task<Result<void>> ws_send(const WebSocket &ws, Str msg);

// The next message, or Err(Closed) once the peer has gone.
Task<Result<String>> ws_recv(const WebSocket &ws);

// A WebSocket broadcast server for the chat demo, in plain Node with no
// dependencies. Two tabs running `chat ws://localhost:8081` talk to each other,
// so the demo needs no internet.
//
// Only what the demo uses: text frames, no extensions, no fragmentation beyond
// the 64 KiB the kernel would ever send.

import { createHash } from "node:crypto";
import { createServer } from "node:http";

const PORT = Number(process.env.BRAAM_WS_PORT || 8081);
const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

const clients = new Set();

function accept(key) {
    return createHash("sha1").update(key + GUID).digest("base64");
}

// A server frame is never masked, so the header is two or four bytes and then
// the payload.
function frame(text) {
    const body = Buffer.from(text, "utf8");
    let head;
    if (body.length < 126) {
        head = Buffer.from([0x81, body.length]);
    } else if (body.length < 65536) {
        head = Buffer.alloc(4);
        head[0] = 0x81;
        head[1] = 126;
        head.writeUInt16BE(body.length, 2);
    } else {
        head = Buffer.alloc(10);
        head[0] = 0x81;
        head[1] = 127;
        head.writeBigUInt64BE(BigInt(body.length), 2);
    }
    return Buffer.concat([head, body]);
}

// Returns [opcode, payload, rest] or null when the buffer holds no whole frame.
function unframe(buf) {
    if (buf.length < 2)
        return null;
    const opcode = buf[0] & 0x0f;
    const masked = (buf[1] & 0x80) !== 0;
    let len = buf[1] & 0x7f;
    let at = 2;
    if (len === 126) {
        if (buf.length < 4)
            return null;
        len = buf.readUInt16BE(2);
        at = 4;
    } else if (len === 127) {
        if (buf.length < 10)
            return null;
        len = Number(buf.readBigUInt64BE(2));
        at = 10;
    }
    const mask = masked ? buf.subarray(at, at + 4) : null;
    at += masked ? 4 : 0;
    if (buf.length < at + len)
        return null;

    const body = Buffer.from(buf.subarray(at, at + len));
    if (mask)
        for (let i = 0; i < body.length; i++)
            body[i] ^= mask[i & 3];
    return [opcode, body, buf.subarray(at + len)];
}

const server = createServer((req, res) => {
    res.writeHead(426, { "content-type": "text/plain" });
    res.end("this port speaks WebSocket only\n");
});

server.on("upgrade", (req, socket) => {
    const key = req.headers["sec-websocket-key"];
    if (!key) {
        socket.destroy();
        return;
    }

    socket.write("HTTP/1.1 101 Switching Protocols\r\n" +
        "upgrade: websocket\r\nconnection: Upgrade\r\n" +
        `sec-websocket-accept: ${accept(key)}\r\n\r\n`);
    clients.add(socket);

    let buf = Buffer.alloc(0);
    socket.on("data", (chunk) => {
        buf = Buffer.concat([buf, chunk]);
        for (;;) {
            const got = unframe(buf);
            if (!got)
                return;
            const [opcode, body, rest] = got;
            buf = rest;
            if (opcode === 8) {
                socket.end();
                return;
            }
            if (opcode !== 1)
                continue;
            const out = frame(body.toString("utf8"));
            for (const peer of clients)
                if (peer !== socket)
                    peer.write(out);
        }
    });

    const drop = () => clients.delete(socket);
    socket.on("close", drop);
    socket.on("error", drop);
});

server.listen(PORT, () => {
    console.log(`braam chat server on ws://localhost:${PORT}/`);
});

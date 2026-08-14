// The JS half of the host-services ABI (Concept.md §3.7). One import, `svc`,
// multiplexed by operation exactly as storage is — and one export in the other
// direction, `ref`, which is how a JS object reaches the kernel: the kernel
// hands out a table slot, the host deposits into it, and every later operation
// on that object arrives with the object itself as the fourth argument.
//
// fetch and WebSocket exist in a worker, so they happen here. The clipboard,
// the file picker and the download need the DOM, so those are relayed to the
// page and answered from there.

import { E, Request, statusOf } from "./abi.js";

export const OP = {
    CLOCK: 1,
    DROP: 2,
    FETCH: 3,
    READ: 4,
    WS_OPEN: 5,
    WS_SEND: 6,
    WS_RECV: 7,
    CLIP_READ: 8,
    CLIP_WRITE: 9,
    PICK: 10,
    PICK_NAME: 11,
    PICK_READ: 12,
    SAVE: 13,
    CLIP_WAIT: 14,
    PROC_SPAWN: 15,
    PROC_STEP: 16,
    PROC_KILL: 17,
};

const utf8 = new TextEncoder();

// A request body and a clipboard string arrive as text; a response body and a
// picked file are bytes. Both ends of the wire are byte counts.
function bytesOf(v) {
    if (typeof v === "string")
        return utf8.encode(v);
    if (v instanceof Uint8Array)
        return v;
    return new Uint8Array(v);
}

// The kernel's request spec for a fetch: a method line, header lines, a blank
// line, then the body.
function parseSpec(spec) {
    const nl = spec.indexOf("\n");
    const method = nl < 0 ? "GET" : spec.slice(0, nl);
    const rest = nl < 0 ? "" : spec.slice(nl + 1);
    const sep = rest.indexOf("\n\n");
    const head = sep < 0 ? rest : rest.slice(0, sep);
    const body = sep < 0 ? "" : rest.slice(sep + 2);

    const headers = {};
    for (const line of head.split("\n")) {
        const at = line.indexOf(":");
        if (at > 0)
            headers[line.slice(0, at).trim()] = line.slice(at + 1).trim();
    }
    return { method: method || "GET", headers, body };
}

function packHeaders(res) {
    let out = "";
    res.headers.forEach((value, name) => {
        out += `${name}: ${value}\n`;
    });
    return utf8.encode(out);
}

// A response whose headers have landed but whose body has not. `left` is the
// tail of the last chunk the kernel had no room for.
function makeBody(res) {
    return { reader: res.body ? res.body.getReader() : null, left: null, done: !res.body };
}

async function readBody(s, r) {
    while ((!s.left || s.left.length === 0) && !s.done) {
        const { value, done } = await s.reader.read();
        if (done)
            s.done = true;
        else
            s.left = value;
    }
    if (!s.left || s.left.length === 0) {
        r.set("bufLen", 0);
        r.set("status", 0);
        return;
    }
    s.left = s.left.subarray(r.writeSome(s.left));
}

// A socket plus the messages that arrived while nothing was reading. Every
// event resolves whoever is parked, so a receive never polls.
function makeSocket(url) {
    const s = { ws: new WebSocket(url), queue: [], closed: false, waiters: [] };
    s.ws.binaryType = "arraybuffer";

    const flush = () => {
        const w = s.waiters;
        s.waiters = [];
        for (const resolve of w)
            resolve();
    };

    s.ws.onmessage = (e) => {
        s.queue.push(bytesOf(e.data));
        flush();
    };
    s.ws.onclose = () => {
        s.closed = true;
        flush();
    };
    s.ws.onerror = () => {
        s.closed = true;
        flush();
    };
    return s;
}

function opened(s) {
    return new Promise((resolve, reject) => {
        if (s.ws.readyState === WebSocket.OPEN) {
            resolve();
            return;
        }
        s.ws.addEventListener("open", () => resolve(), { once: true });
        s.ws.addEventListener("error", () => reject({ braam: E.IO }), { once: true });
        s.ws.addEventListener("close", () => reject({ braam: E.IO }), { once: true });
    });
}

// The message at the head of the queue, left there: it is only consumed once
// the kernel has had room for it (abi.js's two-phase reply).
async function peek(s) {
    while (!s.queue.length && !s.closed)
        await new Promise((resolve) => s.waiters.push(resolve));
    return s.queue.length ? s.queue[0] : null;
}

// A clipboard reply is sized twice like every other variable-length one, and
// neither the async API nor a paste can be asked a second time — so the text is
// held until the kernel has room for it.
async function clipboard(r, held, key, get) {
    if (held[key] === null)
        held[key] = await get();
    const bytes = utf8.encode(held[key]);
    r.write(bytes);
    if (r.get("bufLen") !== 0 || bytes.length === 0)
        held[key] = null;
}

// Builds the import. `deposit` hands an object to the kernel's externref table;
// `relay` asks the page for something only the DOM can do; `reply` delivers a
// finished request. As in fs.js, `reply` must never run inside the import call
// itself — every path below goes through a promise, so it cannot.
export function makeSvcImport(mem, deposit, relay, reply, proc) {
    const held = { read: null, wait: null };

    async function perform(r, op, ref) {
        switch (op) {
        case OP.CLOCK: {
            const now = Date.now();
            r.set("flags", 1440 - new Date().getTimezoneOffset());
            r.ok(now >>> 0, Math.floor(now / 4294967296) >>> 0);
            return;
        }

        case OP.FETCH: {
            // A retry only wants more room for the headers; the object is
            // already in the slot, so the request is not made twice.
            let body = ref;
            if (!body) {
                const spec = parseSpec(r.text());
                const init = { method: spec.method, headers: spec.headers };
                if (spec.body.length && spec.method !== "GET" && spec.method !== "HEAD")
                    init.body = spec.body;
                const res = await fetch(r.arg(), init);
                body = makeBody(res);
                body.status = res.status;
                body.headers = packHeaders(res);
                deposit(r.get("ref"), body);
            }
            r.set("resultLo", body.status);
            const cap = r.get("bufCap");
            if (body.headers.length > cap) {
                r.set("bufLen", 0);
                r.set("resultHi", body.headers.length);
                r.set("status", 0);
                return;
            }
            mem.view().set(body.headers, r.get("bufPtr"));
            r.set("bufLen", body.headers.length);
            r.set("resultHi", 0);
            r.set("status", 0);
            return;
        }

        case OP.READ:
            if (!ref) {
                r.fail(E.INVALID);
                return;
            }
            await readBody(ref, r);
            return;

        case OP.WS_OPEN: {
            const s = makeSocket(r.arg());
            await opened(s);
            deposit(r.get("ref"), s);
            r.ok();
            return;
        }

        case OP.WS_SEND:
            if (!ref || ref.closed) {
                r.fail(E.CLOSED);
                return;
            }
            ref.ws.send(r.text());
            r.ok();
            return;

        case OP.WS_RECV: {
            if (!ref) {
                r.fail(E.INVALID);
                return;
            }
            const msg = await peek(ref);
            if (msg === null) {
                r.set("flags", 1);
                r.set("bufLen", 0);
                r.set("status", 0);
                return;
            }
            r.write(msg);
            if (r.get("bufLen") === 0 && msg.length > 0)
                return; // no room; it stays queued for the retry
            ref.queue.shift();
            return;
        }

        case OP.CLIP_READ:
            await clipboard(r, held, "read", () => relay({ svc: "clipRead" }));
            return;

        case OP.CLIP_WAIT:
            await clipboard(r, held, "wait", () => relay({ svc: "clipWait" }));
            return;

        case OP.CLIP_WRITE:
            await relay({ svc: "clipWrite", text: r.text() });
            r.ok();
            return;

        case OP.PICK: {
            const files = await relay({ svc: "pick" });
            deposit(r.get("ref"), files);
            r.ok(files.length);
            return;
        }

        case OP.PICK_NAME: {
            const f = ref && ref[r.get("flags")];
            if (!f) {
                r.fail(E.NOTFOUND);
                return;
            }
            r.write(utf8.encode(f.name));
            return;
        }

        case OP.PICK_READ: {
            const f = ref && ref[r.get("flags")];
            if (!f) {
                r.fail(E.NOTFOUND);
                return;
            }
            const off = r.get("resultLo") + r.get("resultHi") * 4294967296;
            r.writeSome(f.bytes.subarray(Math.min(off, f.bytes.length)));
            return;
        }

        case OP.SAVE:
            await relay({ svc: "save", name: r.arg(), bytes: r.bytes() });
            r.ok();
            return;

        // Isolated processes (proc.js). They are ops of this import rather
        // than an import of their own for the reason §2.2 gives: one import
        // per calling convention, and this is that convention.
        case OP.PROC_SPAWN:
            proc.spawn(r);
            return;

        // A step is answered when the process gets there, which at tier 3 is a
        // message away and may be never — a program in a loop between syscalls
        // is killed rather than waited for.
        case OP.PROC_STEP:
            await new Promise((done) => proc.step(r, done));
            return;

        default:
            r.fail(E.UNSUPPORTED);
        }
    }

    return function svc(op, token, req, ref) {
        // Killing a process is told, not asked, and `req` is the pid rather
        // than a record: there is nothing to reply to and nothing to free.
        if (op === OP.PROC_KILL) {
            proc.kill(req >>> 0);
            return;
        }

        // Letting go is told, not asked: there is no reply and no record.
        if (op === OP.DROP) {
            if (ref && ref.ws)
                ref.ws.close();
            else if (ref && ref.reader && !ref.done)
                ref.reader.cancel().catch(() => {});
            return;
        }

        const r = new Request(mem, req);
        perform(r, op, ref)
            .catch((e) => r.fail(statusOf(e)))
            .then(() => reply(token));
    };
}

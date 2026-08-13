// Host services for the tests: the same wire format web/svc.js speaks, over
// canned answers. There is no network in CI and no DOM in Node, so a fetch
// serves out of a Map of URLs, a socket loops back to its peers, and the picker
// hands over a fixed set of files.
//
// Like the storage fake, this answers from inside the import — which no browser
// can do and the kernel cannot tell, because wake() only queues. A receive with
// nothing to deliver is the one operation that really parks: it records the
// request and is answered later, by a send or by the socket being dropped.

import { E, Request, statusOf } from "../web/abi.js";
import { OP } from "../web/svc.js";

const utf8 = new TextEncoder();

// What perform() returns when it has not answered yet.
const PARKED = Symbol("parked");

export class FakeNet {
    constructor() {
        this.routes = new Map([
            ["/hello.txt",
                { status: 200, headers: "content-type: text/plain\n", body: "hi there\n" }],
        ]);
        this.clipboard = "";
        this.picked = [{ name: "notes.txt", bytes: utf8.encode("picked\n") }];
        this.saved = [];    // {name, bytes}
        this.sockets = [];
        this.now = 1782000000000;  // a fixed epoch, so `date` prints the same twice
    }

    reset() {
        this.sockets.length = 0;
        this.saved.length = 0;
    }
}

export function makeFakeSvc(mem, net, kernel) {
    // Answers a parked receive, from a send or from the socket going away.
    function settle(s) {
        if (!s.waiting)
            return;
        const { r, token } = s.waiting;
        s.waiting = null;
        if (s.queue.length) {
            const msg = s.queue[0];
            r.write(msg);
            if (r.get("bufLen") !== 0 || msg.length === 0)
                s.queue.shift();
        } else {
            r.set("flags", 1);
            r.set("bufLen", 0);
            r.set("status", 0);
        }
        kernel().wake(token, 0, 0);
    }

    function perform(r, op, ref, token) {
        switch (op) {
        case OP.CLOCK:
            r.set("flags", 1440);
            r.ok(net.now >>> 0, Math.floor(net.now / 4294967296) >>> 0);
            return;

        case OP.FETCH: {
            // A retry only wants more room for the headers; the object is
            // already in the slot, so the request is not made twice.
            let body = ref;
            if (!body) {
                const route = net.routes.get(r.arg());
                if (!route)
                    throw { braam: E.NOTFOUND };
                body = {
                    status: route.status,
                    headers: utf8.encode(route.headers),
                    left: utf8.encode(route.body),
                };
                kernel().ref(r.get("ref"), body);
            }
            r.set("resultLo", body.status);
            if (body.headers.length > r.get("bufCap")) {
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
            if (!ref)
                return r.fail(E.INVALID);
            ref.left = ref.left.subarray(r.writeSome(ref.left));
            return;

        case OP.WS_OPEN: {
            const s = { url: r.arg(), queue: [], closed: false, waiting: null };
            net.sockets.push(s);
            kernel().ref(r.get("ref"), s);
            r.ok();
            return;
        }

        case OP.WS_SEND: {
            if (!ref || ref.closed)
                return r.fail(E.CLOSED);
            const msg = utf8.encode(r.text());
            for (const peer of net.sockets) {
                if (peer === ref || peer.closed)
                    continue;
                peer.queue.push(msg);
                settle(peer);
            }
            // With no peer, a socket hears itself: one tab is still a chat.
            if (net.sockets.length === 1) {
                ref.queue.push(msg);
                settle(ref);
            }
            r.ok();
            return;
        }

        case OP.WS_RECV:
            if (!ref)
                return r.fail(E.INVALID);
            if (!ref.queue.length && !ref.closed) {
                ref.waiting = { r, token };
                return PARKED;
            }
            ref.waiting = { r, token };
            settle(ref); // answers and wakes in place
            return PARKED;

        case OP.CLIP_READ:
            r.write(utf8.encode(net.clipboard));
            return;

        case OP.CLIP_WRITE:
            net.clipboard = r.text();
            r.ok();
            return;

        case OP.PICK:
            kernel().ref(r.get("ref"), net.picked);
            r.ok(net.picked.length);
            return;

        case OP.PICK_NAME: {
            const f = ref && ref[r.get("flags")];
            if (!f)
                return r.fail(E.NOTFOUND);
            r.write(utf8.encode(f.name));
            return;
        }

        case OP.PICK_READ: {
            const f = ref && ref[r.get("flags")];
            if (!f)
                return r.fail(E.NOTFOUND);
            const off = r.get("resultLo") + r.get("resultHi") * 4294967296;
            r.writeSome(f.bytes.subarray(Math.min(off, f.bytes.length)));
            return;
        }

        case OP.SAVE:
            net.saved.push({ name: r.arg(), bytes: r.bytes() });
            r.ok();
            return;

        default:
            r.fail(E.UNSUPPORTED);
        }
    }

    return function svc(op, token, req, ref) {
        if (op === OP.DROP) {
            if (ref && ref.queue) {
                ref.closed = true;
                settle(ref);
            }
            return;
        }

        const r = new Request(mem, req);
        let answer;
        try {
            answer = perform(r, op, ref, token);
        } catch (e) {
            r.fail(statusOf(e));
        }
        if (answer !== PARKED)
            kernel().wake(token, 0, 0);
    };
}

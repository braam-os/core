// A tier-3 process worker with no worker in it. web/procworker.js needs a
// thread and web/proc.js does not: everything above the link is message
// passing, so the two halves are wired back to back here and pumped by hand,
// exactly as the driver pumps a tier-2 step.
//
// What that proves is the protocol — one message down, one up, the syscall
// relay, the exit status, the pool — and what it cannot prove is preemption,
// because Node is as single-threaded as the browser's kernel worker. So a
// program that loops is modelled rather than run: `hold` leaves a step
// undelivered, which is precisely what the kernel sees of a real one, and the
// terminate that answers it is counted.

import { serveProc, workerOps, STEP } from "../web/proc.js";

// The clock both ends share. Frozen, because the driver's timestamps are
// literals and a test that watched the wall clock would not be a test.
const CLOCK = () => 0;

export function makeFakeLinks(net) {
    const links = [];

    net.links = links;
    net.terminated = [];   // one entry per worker killed rather than pooled
    net.bound = [];        // the pid of every process that ran in one
    net.workers = true;    // false makes a link refuse to be made
    net.broken = false;    // true makes one that never loads: onerror, no ready
    net.held = new Set();  // pids whose steps sit undelivered, as a loop does
    net.holdIn = 0;        // binds to let by before holding one; 0 is off

    // A pid is not known until the kernel has one, and by then the command has
    // been submitted — so a test counts binds and the bind fills the pid in.
    // Every program takes a worker now, so a pipeline binds one per stage and a
    // spawning program binds before its child: `n` is which of them to hold.
    net.hold = (n = 1) => {
        net.holdIn = n;
    };

    net.release = () => {
        net.held.clear();
        net.holdIn = 0;
    };

    // One round of message passing, in both directions. Returns true when
    // anything moved, so the driver can loop until nothing does.
    net.pump = () => {
        let moved = false;
        for (const link of links.slice()) {
            if (link.dead)
                continue;

            // Down: the kernel's messages, one at a time, since a step's
            // answer may kill the link the next one was meant for.
            while (link.down.length) {
                if (link.down[0].k === "step" && net.held.has(link.pid))
                    break;
                link.serve(link.down.shift());
                moved = true;
            }

            // Up: what the worker said, delivered where a real one would
            // deliver it — off the kernel's stack. A worker that failed to
            // load says it there too, through the other handler.
            while (link.up.length && !link.dead) {
                const m = link.up.shift();
                if (m.k === "error")
                    link.onerror();
                else
                    link.onmessage({ data: m });
                moved = true;
            }
        }
        return moved;
    };

    return function makeFakeLink() {
        if (!net.workers)
            throw new Error("no nested workers here");

        const link = {
            pid: 0,
            down: [],
            up: [],
            dead: false,
            onmessage: null,
            onerror: null,

            postMessage(msg) {
                link.down.push(msg);
            },

            terminate() {
                link.dead = true;
                net.terminated.push(link);
            },
        };

        // web/procworker.js, without the `self` it would talk through.
        let ops = null;
        let server = null;

        link.serve = (m) => {
            if (m.k === "bind") {
                net.bound.push(m.pid);
                if (net.holdIn && --net.holdIn === 0)
                    net.held.add(m.pid);
                ops = workerOps(m.pid, CLOCK);
                server = serveProc(ops);
                try {
                    server.bind(m.module, m.initial, m.maximum);
                } catch {
                    server = null;
                }
                return;
            }
            if (m.k !== "step")
                return;
            if (!server) {
                link.up.push({ k: "step", result: STEP.TRAPPED });
                return;
            }
            ops.begin(m.now);
            const out = server.step(m.token, new Uint8Array(m.payload));
            const { exit, calls } = ops.end();
            link.up.push({ k: "step", ...out, exit, calls });
        };

        links.push(link);

        // A worker whose script never ran: it serves nothing and reports the
        // failure the only way a real one can, which is the error event.
        if (net.broken) {
            link.serve = () => {};
            link.up.push({ k: "error" });
        } else {
            link.up.push({ k: "ready" });
        }
        return link;
    };
}

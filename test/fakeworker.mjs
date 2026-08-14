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
    net.held = new Set();  // pids whose steps sit undelivered, as a loop does
    net.holdNext = false;  // hold the next process to be bound, whatever pid

    // A pid is not known until the kernel has one, and by then the command has
    // been submitted — so a test says "the next one" and the bind fills it in.
    net.hold = () => {
        net.holdNext = true;
    };

    net.release = () => {
        net.held.clear();
        net.holdNext = false;
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
            // deliver it — off the kernel's stack.
            while (link.up.length && !link.dead) {
                link.onmessage({ data: link.up.shift() });
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
                if (net.holdNext) {
                    net.held.add(m.pid);
                    net.holdNext = false;
                }
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
        link.up.push({ k: "ready" });
        return link;
    };
}

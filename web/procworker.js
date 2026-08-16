// A process lives here: one worker, one instance, and no kernel
// (Concept.md §4). Everything it does is in web/proc.js, so that both halves of
// the step protocol are written in one file; this is the wiring only.

import { serveProc, workerOps, STEP } from "./proc.js";

let ops = null;
let server = null;

self.onmessage = ({ data }) => {
    if (data.k === "bind") {
        ops = workerOps(data.pid, () => performance.now());
        server = serveProc(ops);
        try {
            server.bind(data.module, data.initial, data.maximum);
        } catch {
            // There is nobody to tell yet: the kernel learns at the first step,
            // as it would for a process that trapped on its first instruction.
            server = null;
        }
        return;
    }

    if (data.k !== "step")
        return;

    if (!server) {
        self.postMessage({ k: "step", result: STEP.TRAPPED });
        return;
    }

    ops.begin(data.now);
    const out = server.step(data.token, new Uint8Array(data.payload));
    const { exit, calls } = ops.end();
    self.postMessage({ k: "step", ...out, exit, calls }, calls.map((c) => c.payload));
};

self.postMessage({ k: "ready" });

// Headless driver for kernel.wasm and tests.wasm. Node stands in for the
// browser: instantiating a freestanding module needs nothing browser-specific.

import { readFileSync } from "node:fs";
import { basename } from "node:path";

function usage() {
    console.error("usage: run.mjs --kernel <wasm> | --tests <wasm>");
    process.exit(2);
}

const [mode, file] = process.argv.slice(2);
if (!file || (mode !== "--kernel" && mode !== "--tests"))
    usage();

const logged = [];
let memory = null;
let view = null;

// memory.grow detaches the buffer (Concept.md §8.4), so re-derive on demand.
function bytes() {
    if (view === null || view.byteLength === 0)
        view = new Uint8Array(memory.buffer);
    return view;
}

const imports = {
    host: {
        log(ptr, len) {
            const text = new TextDecoder().decode(bytes().subarray(ptr, ptr + len));
            logged.push(text);
            console.log(text);
        },
        now: () => performance.now(),
    },
};

const module = new WebAssembly.Module(readFileSync(file));
const instance = new WebAssembly.Instance(module, imports);
memory = instance.exports.memory;

const fail = (msg) => {
    console.error(`${basename(file)}: ${msg}`);
    process.exit(1);
};

const names = (list) => list.map((e) => `${e.module ? e.module + "." : ""}${e.name}`).sort();

if (mode === "--kernel") {
    // The import and export surface is the ABI; drift is a bug, and an
    // unexpected import means a libc dependency crept in.
    const want_imports = ["host.log", "host.now"];
    const want_exports = ["init", "memory", "tick", "wake"];
    const got_imports = names(WebAssembly.Module.imports(module));
    const got_exports = names(WebAssembly.Module.exports(module));

    if (got_imports.join() !== want_imports.join())
        fail(`imports are [${got_imports}], expected [${want_imports}]`);
    if (got_exports.join() !== want_exports.join())
        fail(`exports are [${got_exports}], expected [${want_exports}]`);

    instance.exports.init(0);

    if (logged.length !== 1)
        fail(`expected one boot line, got ${logged.length}`);
    if (!logged[0].startsWith("braam "))
        fail(`unexpected boot line: ${logged[0]}`);

    // The demo tasks run only when ticked, on a clock we supply, so their
    // interleaving is exact rather than approximate. a sleeps 10 then 20,
    // b sleeps 15 then 10.
    const delays = [0, 10, 15, 25, 30].map((now) => instance.exports.tick(now));
    const want_delays = [10, 5, 10, 5, -1];
    if (delays.join() !== want_delays.join())
        fail(`tick returned [${delays}], expected [${want_delays}]`);

    const order = logged.slice(1).join(" ");
    if (order !== "demo a1 demo b1 demo b2 demo a2")
        fail(`the demo tasks ran out of order: ${order}`);

    console.log(`smoke ok: ${got_imports.length} imports, ${got_exports.length} exports`);
} else {
    const failures = instance.exports.run_tests();
    if (failures !== 0)
        fail(`${failures} check(s) failed`);
}

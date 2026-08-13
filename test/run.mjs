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
const presented = [];
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
        present(x, y, w, h) {
            presented.push({ x, y, w, h });
        },
    },
};

// The screen descriptor, as u32s, in the order src/kernel/screen.h declares.
const SCREEN = ["magic", "cols", "rows", "cursor_x", "cursor_y", "cursor_on", "cells"];
const SCREEN_MAGIC = 0x42534352;

function descriptor(addr) {
    const u32 = new Uint32Array(memory.buffer, addr, SCREEN.length);
    return Object.fromEntries(SCREEN.map((name, i) => [name, u32[i]]));
}

// One cell is {ch: u32, fg|bg|attrs|pad: u32} — the layout render.js assumes.
function cell(s, x, y) {
    const u32 = new Uint32Array(memory.buffer, s.cells + (y * s.cols + x) * 8, 2);
    return { ch: u32[0], fg: u32[1] & 0xff, bg: (u32[1] >>> 8) & 0xff };
}

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
    const want_imports = ["host.log", "host.now", "host.present"];
    const want_exports = ["init", "key", "memory", "resize", "tick", "wake"];
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

    // M2, first criterion: a typed character reaches a cell and the cursor
    // moves. resize() hands back the descriptor; nothing else tells JS where
    // the grid is.
    const addr = instance.exports.resize(20, 5);
    if (addr === 0)
        fail("resize returned no screen descriptor");

    let s = descriptor(addr);
    if (s.magic !== SCREEN_MAGIC)
        fail(`screen magic is ${s.magic.toString(16)}, expected ${SCREEN_MAGIC.toString(16)}`);
    if (s.cols !== 20 || s.rows !== 5)
        fail(`resize(20, 5) gave ${s.cols}x${s.rows}`);

    // A resize repaints everything, and the host ticks to let it out.
    presented.length = 0;
    instance.exports.tick(35);
    if (presented.length !== 1 || presented[0].w !== 20 || presented[0].h !== 5)
        fail(`the resize did not repaint the whole screen: ${JSON.stringify(presented)}`);

    const home = s.cursor_y; // the banner left the cursor on its own line
    presented.length = 0;
    for (const ch of "hi")
        instance.exports.key(ch.codePointAt(0), 0);
    instance.exports.tick(40);

    s = descriptor(addr);
    if (cell(s, 0, home).ch !== 0x68 || cell(s, 1, home).ch !== 0x69)
        fail("the typed characters did not reach the cells");
    if (s.cursor_x !== 2)
        fail(`the cursor is at column ${s.cursor_x}, expected 2`);

    // One present per tick, covering the two written cells and both cursor
    // positions — the cell it left must repaint, or it leaves a ghost.
    if (presented.length !== 1)
        fail(`expected one present, got ${presented.length}`);
    const r = presented[0];
    if (r.x !== 0 || r.y !== home || r.w !== 3 || r.h !== 1)
        fail(`present rect is ${r.x},${r.y} ${r.w}x${r.h}, expected 0,${home} 3x1`);

    // M2, second criterion: resize reflows, keeping the bottom of the screen.
    if (instance.exports.resize(20, 2) === 0)
        fail("the reflowing resize failed");
    s = descriptor(addr);
    if (s.cols !== 20 || s.rows !== 2)
        fail(`resize(20, 2) gave ${s.cols}x${s.rows}`);
    if (cell(s, 0, s.cursor_y).ch !== 0x68 || cell(s, 1, s.cursor_y).ch !== 0x69)
        fail("the reflow did not keep the typed line");
    if (s.cursor_x !== 2 || s.cursor_y >= s.rows)
        fail(`the cursor left the grid at ${s.cursor_x},${s.cursor_y}`);

    // A geometry the kernel will not honour is clamped, and reported back.
    s = descriptor(instance.exports.resize(9999, 9999));
    if (s.cols !== 512 || s.rows !== 256)
        fail(`an oversized resize gave ${s.cols}x${s.rows}`);

    console.log(`smoke ok: ${got_imports.length} imports, ${got_exports.length} exports`);
} else {
    const failures = instance.exports.run_tests();
    if (failures !== 0)
        fail(`${failures} check(s) failed`);
}

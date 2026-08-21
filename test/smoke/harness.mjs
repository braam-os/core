// The smoke suite's driver: one kernel, one screen, one shell, and the readers
// every case reads them with. Node stands in for the browser — instantiating a
// freestanding module needs nothing browser-specific, and test/fakefs.mjs
// stands in for OPFS.
//
// The state here is deliberately a single instance rather than a value handed
// to each case: the suite is one cumulative session, and several cases assert
// absolute running totals against `logged`, `presented` and `store.unpacks`.
// test/run.mjs calls init() once and then the cases in order; doc/Testing.md
// has the rules they run by.

import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { basename } from "node:path";

import { FakeStore, makeFakeImports } from "../fakefs.mjs";
import { parseZip } from "../../web/fs.js";
import { FakeNet, makeFakeSvc } from "../fakesvc.mjs";

export const logged = [];
export const presented = [];
export const store = new FakeStore();
export const net = new FakeNet();

let memory = null;
let view = null;
let u32 = null;
let instance = null;
let module = null;
let name = "run.mjs";
let addr = 0;
let tick_count = 0;

// Whether the archive came along: the cases that read /share or /pkg are
// skipped without it, and run.mjs gates the rest off the same flag.
export let hasRootfs = false;

// memory.grow detaches the buffer (Concept.md §8.4), so re-derive on demand.
function bytes() {
    if (view === null || view.byteLength === 0)
        view = new Uint8Array(memory.buffer);
    return view;
}

// The shim web/fs.js expects, so the fake backend can share its encoders.
// web/render.js takes the same shim, which is why it is exported.
export const mem = {
    view: bytes,
    u32() {
        if (u32 === null || u32.byteLength === 0)
            u32 = new Uint32Array(memory.buffer);
        return u32;
    },
    str(ptr, len) {
        return new TextDecoder().decode(bytes().subarray(ptr, ptr + len));
    },
};

const imports = {
    host: {
        log(ptr, len) {
            const text = mem.str(ptr, len);
            logged.push(text);
            console.log(text);
        },
        now: () => performance.now(),
        present(x, y, w, h) {
            presented.push({ x, y, w, h });
        },
        ...makeFakeImports(mem, store, () => instance.exports),
        svc: makeFakeSvc(mem, net, () => instance.exports),
    },
};

// One stored (uncompressed) entry, by hand: pack.py always deflates, so this
// is the only exercise method 0 gets, and it is what lets a hostile name be
// put in front of the parser without a packer that would refuse to write one.
function zipOf(entry, body) {
    const n = new TextEncoder().encode(entry);
    const data = new TextEncoder().encode(body);
    const out = [];
    const put = (...b) => out.push(...b);
    const u16 = (v) => put(v & 0xff, (v >> 8) & 0xff);
    const w32 = (v) => { u16(v & 0xffff); u16((v >>> 16) & 0xffff); };

    w32(0x04034b50); u16(20); u16(0); u16(0); u16(0); u16(0);
    w32(0); w32(data.length); w32(data.length); u16(n.length); u16(0);
    put(...n, ...data);

    const cd = out.length;
    w32(0x02014b50); u16(20); u16(20); u16(0); u16(0); u16(0); u16(0);
    w32(0); w32(data.length); w32(data.length);
    u16(n.length); u16(0); u16(0); u16(0); u16(0); w32(0); w32(0);
    put(...n);

    const end = out.length;
    w32(0x06054b50); u16(0); u16(0); u16(1); u16(1);
    w32(end - cd); w32(cd); u16(0);
    return new Uint8Array(out);
}

// Compiles the kernel, wires the fakes, and reads the archive. Asynchronous
// because inflating is: the fake answers synchronously from inside the import,
// so the phase that can await does it once, here.
export async function init(wasm, rootfs, mode) {
    name = basename(wasm);
    module = new WebAssembly.Module(readFileSync(wasm));
    hasRootfs = Boolean(rootfs);

    if (rootfs) {
        const archive = new Uint8Array(readFileSync(rootfs));
        store.entries = await parseZip(archive);

        // The archive itself, and what web/fs.js made of it: src/cmd/pkg/zip.cpp
        // reads the same bytes in wasm and test_zip compares the two entry for
        // entry, so neither reader is trusted against its own idea of the format.
        // A digest rather than the bytes, so nothing large is held twice. Only for
        // the unit suite: the smoke test reads the root and would find them.
        if (mode === "--tests") {
            store.files.set("/rootfs.zip", archive);
            store.files.set("/rootfs.manifest", new TextEncoder().encode(
                store.entries.map((e) => `${e.name} ${e.bytes.length} ` +
                    createHash("sha256").update(e.bytes).digest("hex")).join("\n") + "\n"));
        }
    }

    const stored = await parseZip(zipOf("share/hello", "hi"));
    if (stored.length !== 1 || new TextDecoder().decode(stored[0].bytes) !== "hi")
        throw new Error(`a stored entry did not read back: ${JSON.stringify(stored)}`);

    // Concept.md §5.2's store is the whole namespace now, so a name that escapes
    // the root is the one zip bug worth checking by hand.
    for (const escape of ["../escape", "/etc/passwd", "bin/../../out", "C:\\out"]) {
        let refused = false;
        await parseZip(zipOf(escape, "x")).catch(() => { refused = true; });
        if (!refused)
            throw new Error(`the unpacker accepted ${JSON.stringify(escape)}`);
    }

    instantiate();
}

export function instantiate() {
    // A reload throws the kernel worker away and every nested worker with it,
    // which is what `shutdown` is (web/proc.js). The process table is the
    // *host's* and outlives the kernel here, so without this the outgoing
    // shell's entry is overwritten by the incoming one at the same pid and the
    // worker behind it is orphaned — never pooled, never terminated, and still
    // counted. Harmless before the first boot, when there is nothing in it.
    net.proc.shutdown();
    instance = new WebAssembly.Instance(module, imports);
    memory = instance.exports.memory;
    view = null;
    u32 = null;
    return instance;
}

// The kernel's exports, re-read every time: instantiate() replaces them.
export const kernel = () => instance.exports;

// The module itself, for the case that asserts its import and export surface.
export const compiled = () => module;

export const fail = (msg) => {
    console.error(`${name}: ${msg}`);
    process.exit(1);
};

export const names = (list) =>
    list.map((e) => `${e.module ? e.module + "." : ""}${e.name}`).sort();

// A tick, and then whatever the host owes a process: a step is a message in a
// browser and a hand-pumped call here, so the driver runs what the worker's
// event loop would have run. Returns the last delay tick reported, so every
// assertion about the timer queue still reads the same.
//
// A delay of 0 means the kernel has ready work and wants running again at once,
// which web/worker.js answers with setTimeout(pump, 0). Looping on it here is
// the same rule: a wake issued while the scheduler sweeps its finished jobs —
// a process reporting its exit status to its parent, say — lands on the ready
// queue after the drain that would have run it.
export function run(now) {
    let delay = instance.exports.tick(now);
    tick_count++;
    while (net.drain() || delay === 0) {
        delay = instance.exports.tick(now);
        tick_count++;
    }
    return delay;
}

// The tick count is a running total the present-accounting case zeroes first.
export const ticks = () => tick_count;
export const resetTicks = () => { tick_count = 0; };

// The shell is an instance now, and a live one for as long as the system is up
// — a worker of its own with it — so what every assertion below means by "live"
// is "besides the shell". Subtracting it here rather than at sixteen call sites
// keeps them saying what they were written to say. It is exactly one: init
// replaces a shell that died before anything else runs, so the window in which
// there is none is inside a single `run()`.
export const others = () => net.proc.live() - 1;

// The screen descriptor, as u32s, in the order src/kernel/screen.h declares.
const SCREEN = ["magic", "cols", "rows", "cursor_x", "cursor_y", "cursor_on", "cells"];
export const SCREEN_MAGIC = 0x42534352;

export function descriptor(at) {
    const w = new Uint32Array(memory.buffer, at, SCREEN.length);
    return Object.fromEntries(SCREEN.map((field, i) => [field, w[i]]));
}

// resize() hands back the descriptor's address and nothing else tells JS where
// the grid is, so it is kept here rather than in a case: a case that resizes
// hands the next one a grid at a new address, and `submit` below reads it. The
// convention is that a case restores 60x16 before it returns. Returns what the
// export returned, 0 included, since a refused geometry is asserted on.
export function resize(cols, rows_) {
    addr = kernel().resize(cols, rows_);
    return addr;
}

export const screen = () => descriptor(addr);

// The address itself, which only web/render.js's attach() has any use for.
export const gridAddr = () => addr;

// A resize with the refusal checked, since a geometry the kernel will not
// honour hands back 0 and every reader after it would be reading address zero.
export function regrid(cols, rows_, why) {
    if (resize(cols, rows_) === 0)
        fail(why);
    return screen();
}

// One cell is {ch: u32, fg|bg|attrs|pad: u32} — the layout render.js assumes.
// `cells` moves when a scrollback view opens or closes, so a descriptor taken
// before a keystroke must not be read after one.
export function cell(s, x, y) {
    const w = new Uint32Array(memory.buffer, s.cells + (y * s.cols + x) * 8, 2);
    return { ch: w[0], fg: w[1] & 0xff, bg: (w[1] >>> 8) & 0xff };
}

// One row as text, trailing blanks trimmed.
export function row(s, y) {
    let out = "";
    for (let x = 0; x < s.cols; x++) {
        const ch = cell(s, x, y).ch;
        out += ch ? String.fromCodePoint(ch) : " ";
    }
    return out.replace(/ +$/, "");
}

export function rows(s) {
    return Array.from({ length: s.rows }, (_, y) => row(s, y));
}

// The names in a listing: ls pads to a shared column width when stdout is the
// grid, so a name is a field of a row rather than a row of its own.
export function words(s) {
    return rows(s).flatMap((line) => line.split(/ +/).filter(Boolean));
}

// What one command printed: the rows between the line it was typed on and the
// prompt that followed, blank lines kept — which is what `ls -R` separates its
// blocks with. Meant for a screen that was cleared first.
export function output(s) {
    const all = rows(s);
    const first = all.findIndex((line) => line.includes("$ ")) + 1;
    let last = all.length;
    for (let i = first; i < all.length; i++)
        if (all[i].endsWith("$")) {
            last = i;
            break;
        }
    return all.slice(first, last);
}

// The prompt is the last status in red, the cwd's basename white on blue, then
// the $ in bright white. The suite cds, so it tracks where the shell in front
// of it is; the boot cwd is /home.
let cwd = "/home";

export const chdir = (path) => { cwd = path; };

export function prompt(status = 0) {
    const base = cwd.slice(cwd.lastIndexOf("/") + 1) || "/";
    return `${status ? `[${status}] ` : ""}${base} $`;
}

// Named keys, from the enum in src/kernel/key.h — keep the two in step.
const NAMED = 0x110000;
export const KEY = {
    ENTER: NAMED, BACKSPACE: NAMED + 1, TAB: NAMED + 2, ESCAPE: NAMED + 3,
    DELETE: NAMED + 4, UP: NAMED + 6, DOWN: NAMED + 7, LEFT: NAMED + 8,
    RIGHT: NAMED + 9, HOME: NAMED + 10, END: NAMED + 11, PAGE_UP: NAMED + 12,
    PAGE_DOWN: NAMED + 13,
};
export const CTRL = 2;
export const SHIFT = 1;

export const type = (text) => {
    for (const ch of text)
        instance.exports.key(ch.codePointAt(0), 0);
};

export const press = (code, mods = 0) => instance.exports.key(code, mods);

export const submit = (text, now) => {
    type(text);
    press(KEY.ENTER);
    run(now);
    return descriptor(addr);
};

// A clock and the two shapes almost every case asserts in: a cleared screen,
// one line, and what it printed. `is` compares the whole output joined with |,
// `has` looks for one row among all of them. Nine near-identical copies of
// these lived in the one file this suite was split out of.
export function shows(base, step = 0.005) {
    let t = base;
    // A case that wants a wider gap for one line passes its own step.
    const at = (by = step) => (t += by);
    const line = (text) => submit(text, at());
    return {
        at,
        line,
        is(text, want) {
            line("clear");
            const got = output(line(text)).join("|");
            if (got !== want)
                fail(`\`${text}\` printed ${JSON.stringify(got)}, ` +
                     `expected ${JSON.stringify(want)}`);
        },
        has(text, want) {
            line("clear");
            const got = rows(line(text));
            if (!got.includes(want))
                fail(`${text} printed ${JSON.stringify(got)}, expected ${want}`);
        },
    };
}

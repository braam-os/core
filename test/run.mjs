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

// One row as text, trailing blanks trimmed.
function row(s, y) {
    let out = "";
    for (let x = 0; x < s.cols; x++) {
        const ch = cell(s, x, y).ch;
        out += ch ? String.fromCodePoint(ch) : " ";
    }
    return out.replace(/ +$/, "");
}

function rows(s) {
    return Array.from({ length: s.rows }, (_, y) => row(s, y));
}

// Named keys, from the enum in src/kernel/key.h — keep the two in step.
const NAMED = 0x110000;
const KEY = { ENTER: NAMED, BACKSPACE: NAMED + 1, UP: NAMED + 6, HOME: NAMED + 10 };
const CTRL = 2;

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

    // init spawns the shell and nothing else. The first tick draws its prompt
    // and parks it on the keyboard, so there is nothing left pending.
    if (instance.exports.tick(0) !== -1)
        fail("the shell did not park on the keyboard");

    // resize() hands back the descriptor; nothing else tells JS where the
    // grid is.
    const addr = instance.exports.resize(60, 16);
    if (addr === 0)
        fail("resize returned no screen descriptor");

    let s = descriptor(addr);
    if (s.magic !== SCREEN_MAGIC)
        fail(`screen magic is ${s.magic.toString(16)}, expected ${SCREEN_MAGIC.toString(16)}`);
    if (s.cols !== 60 || s.rows !== 16)
        fail(`resize(60, 16) gave ${s.cols}x${s.rows}`);

    // A resize repaints everything, and the host ticks to let it out.
    presented.length = 0;
    instance.exports.tick(1);
    if (presented.length !== 1 || presented[0].w !== 60 || presented[0].h !== 16)
        fail(`the resize did not repaint the whole screen: ${JSON.stringify(presented)}`);

    const type = (text) => {
        for (const ch of text)
            instance.exports.key(ch.codePointAt(0), 0);
    };
    const press = (code, mods = 0) => instance.exports.key(code, mods);
    const submit = (text, now) => {
        type(text);
        press(KEY.ENTER);
        instance.exports.tick(now);
        return descriptor(addr);
    };

    // M1's coverage, now supplied by the shell instead of by demo tasks:
    // `sleep` parks on the timer queue, and the delays tick reports are exact
    // because the clock is ours. It exercises argv and the registry with it.
    type("sleep 30");
    press(KEY.ENTER);
    const delays = [1000, 1010, 1030].map((now) => instance.exports.tick(now));
    const want_delays = [30, 20, -1];
    if (delays.join() !== want_delays.join())
        fail(`tick returned [${delays}], expected [${want_delays}]`);

    // M3, first criterion: `echo hello` prints and `help` lists the programs.
    s = submit("echo hello", 1040);
    if (!rows(s).includes("hello"))
        fail(`echo did not print: ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== "$")
        fail(`the prompt after a success is ${row(s, s.cursor_y)}, expected $`);

    submit("clear", 1045); // thirteen programs need the whole grid
    s = submit("help", 1050);
    for (const name of ["cat", "clear", "echo", "false", "grep", "head", "help", "ls",
                        "sleep", "tail", "true", "version", "wc"])
        if (!rows(s).some((line) => line.startsWith(`  ${name} `)))
            fail(`help did not list ${name}: ${JSON.stringify(rows(s))}`);

    // M3, second criterion, first half: a nonzero exit code is observable —
    // the shell carries it in the next prompt.
    s = submit("false", 1060);
    if (!rows(s).includes("[1] $"))
        fail(`a failing program left ${row(s, s.cursor_y)}, expected [1] $`);

    s = submit("nosuch", 1070);
    if (!rows(s).some((line) => line.startsWith("braam: nosuch: not found")))
        fail(`an unknown command said nothing: ${JSON.stringify(rows(s))}`);
    if (!rows(s).includes("[127] $"))
        fail(`an unknown command left ${row(s, s.cursor_y)}, expected [127] $`);

    s = submit("true", 1080);
    if (row(s, s.cursor_y) !== "$")
        fail(`a succeeding program left ${row(s, s.cursor_y)}, expected $`);

    // M3, second criterion, second half: Up recalls history, Home reaches the
    // start of the recalled line, and ^C abandons it.
    press(KEY.UP);
    instance.exports.tick(1090);
    s = descriptor(addr);
    if (!row(s, s.cursor_y).endsWith("true"))
        fail(`Up recalled ${row(s, s.cursor_y)}, expected it to end in true`);

    press(KEY.HOME);
    instance.exports.tick(1100);
    s = descriptor(addr);
    if (s.cursor_x !== 2)
        fail(`Home left the cursor at column ${s.cursor_x}, expected 2`);

    press("c".codePointAt(0), CTRL);
    instance.exports.tick(1110);
    s = descriptor(addr);
    if (!rows(s).includes("[130] $"))
        fail(`^C left ${row(s, s.cursor_y)}, expected [130] $`);

    // M2's coverage: one present per tick, covering every cell the editor drew
    // and the cell the cursor left — that one must repaint or it ghosts.
    presented.length = 0;
    const x0 = s.cursor_x;
    const y0 = s.cursor_y;
    type("hi");
    instance.exports.tick(1120);
    s = descriptor(addr);
    if (s.cursor_x !== x0 + 2)
        fail(`the cursor is at column ${s.cursor_x}, expected ${x0 + 2}`);
    if (presented.length !== 1)
        fail(`expected one present, got ${presented.length}`);
    const r = presented[0];
    if (r.x > x0 || r.y > y0 || r.x + r.w < s.cursor_x + 1 || r.y + r.h <= y0)
        fail(`present rect ${r.x},${r.y} ${r.w}x${r.h} misses ${x0}..${s.cursor_x},${y0}`);

    // M2's second criterion: resize reflows, keeping the rows in use.
    if (instance.exports.resize(20, 2) === 0)
        fail("the reflowing resize failed");
    s = descriptor(addr);
    if (s.cols !== 20 || s.rows !== 2)
        fail(`resize(20, 2) gave ${s.cols}x${s.rows}`);
    if (!row(s, s.cursor_y).endsWith("hi"))
        fail(`the reflow lost the line being edited: ${row(s, s.cursor_y)}`);
    if (s.cursor_y >= s.rows)
        fail(`the cursor left the grid at ${s.cursor_x},${s.cursor_y}`);

    // A geometry the kernel will not honour is clamped, and reported back.
    s = descriptor(instance.exports.resize(9999, 9999));
    if (s.cols !== 512 || s.rows !== 256)
        fail(`an oversized resize gave ${s.cols}x${s.rows}`);

    // M4, first criterion: a pipeline, in the shipping kernel. `ls` lists the
    // registry — what /bin will hold — and grep filters it, both running at
    // once over a bounded pipe. `clear` first, so the rows below are the
    // pipeline's and nothing else's.
    if (instance.exports.resize(60, 16) === 0)
        fail("the resize before the pipeline failed");
    press("c".codePointAt(0), CTRL); // the "hi" typed above is still pending
    s = submit("clear", 1130);
    s = submit("ls | grep hel", 1140);
    const listed = rows(s).filter((line) => line && !line.includes("$"));
    if (listed.join() !== "help")
        fail(`ls | grep hel printed ${JSON.stringify(listed)}, expected ["help"]`);
    if (row(s, s.cursor_y) !== "$")
        fail(`a pipeline that matched left ${row(s, s.cursor_y)}, expected $`);

    // The status of a pipeline is its last command's: grep reports 1 when
    // nothing matched, and quote removal reaches argv on the way in.
    s = submit("ls | grep zzz", 1150);
    if (!rows(s).includes("[1] $"))
        fail(`an empty pipeline left ${row(s, s.cursor_y)}, expected [1] $`);
    s = submit("echo 'a b' | wc", 1160);
    if (!rows(s).includes("1 2 4"))
        fail(`echo 'a b' | wc printed ${JSON.stringify(rows(s))}, expected 1 2 4`);

    // Redirection parses but has nowhere to go until M5, and nothing runs.
    s = submit("echo hi > out.txt", 1170);
    if (!rows(s).some((line) => line.startsWith("braam: out.txt: no filesystem")))
        fail(`a redirection said nothing: ${JSON.stringify(rows(s))}`);
    if (rows(s).includes("hi"))
        fail("a command with an impossible redirection ran anyway");

    // M4, second criterion: ^C interrupts a running pipeline and the prompt
    // comes back. tick's return value is what proves the sleep really went.
    type("sleep 5000");
    press(KEY.ENTER);
    if (instance.exports.tick(1180) !== 5000)
        fail("the pipeline did not park on the timer");
    press("c".codePointAt(0), CTRL);
    if (instance.exports.tick(1190) !== -1)
        fail("^C left the pipeline's timer armed");
    s = descriptor(addr);
    if (!rows(s).includes("[130] $"))
        fail(`^C on a pipeline left ${row(s, s.cursor_y)}, expected [130] $`);

    // The keyboard came back: the pump was its only receiver while the job
    // ran, and the shell has to be able to read it again afterwards.
    s = submit("echo back", 1200);
    if (!rows(s).includes("back"))
        fail(`the shell lost the keyboard after ^C: ${JSON.stringify(rows(s))}`);

    console.log(`smoke ok: ${got_imports.length} imports, ${got_exports.length} exports`);
} else {
    const failures = instance.exports.run_tests();
    if (failures !== 0)
        fail(`${failures} check(s) failed`);
}

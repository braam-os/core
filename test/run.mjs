// Headless driver for kernel.wasm and tests.wasm. Node stands in for the
// browser: instantiating a freestanding module needs nothing browser-specific,
// and test/fakefs.mjs stands in for OPFS.

import { readFileSync } from "node:fs";
import { basename } from "node:path";

import { E } from "../web/abi.js";
import { FakeStore, makeFakeImports } from "./fakefs.mjs";
import { FakeNet, makeFakeSvc } from "./fakesvc.mjs";
import { pasted } from "../web/keys.js";
import { Renderer } from "../web/render.js";

function usage() {
    console.error("usage: run.mjs --kernel <wasm> [<bundle.bin> [<proc.wasm>...]] |" +
                  " --tests <wasm>");
    process.exit(2);
}

const [mode, file, bundle] = process.argv.slice(2);
const binaries = process.argv.slice(5);
if (!file || (mode !== "--kernel" && mode !== "--tests"))
    usage();

const logged = [];
const presented = [];
let memory = null;
let view = null;
let u32 = null;
let instance = null;

// memory.grow detaches the buffer (Concept.md §8.4), so re-derive on demand.
function bytes() {
    if (view === null || view.byteLength === 0)
        view = new Uint8Array(memory.buffer);
    return view;
}

// The shim web/fs.js expects, so the fake backend can share its encoders.
const mem = {
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

const store = new FakeStore();
const net = new FakeNet();

// The archive tools/pack.py just produced, so the packer and src/fs/bundlefs.cpp
// are checked against each other rather than each against its own idea.
if (bundle)
    store.bundle = new Uint8Array(readFileSync(bundle));

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

// The screen descriptor, as u32s, in the order src/kernel/screen.h declares.
const SCREEN = ["magic", "cols", "rows", "cursor_x", "cursor_y", "cursor_on", "cells"];
const SCREEN_MAGIC = 0x42534352;

function descriptor(addr) {
    const words = new Uint32Array(memory.buffer, addr, SCREEN.length);
    return Object.fromEntries(SCREEN.map((name, i) => [name, words[i]]));
}

// One cell is {ch: u32, fg|bg|attrs|pad: u32} — the layout render.js assumes.
function cell(s, x, y) {
    const words = new Uint32Array(memory.buffer, s.cells + (y * s.cols + x) * 8, 2);
    return { ch: words[0], fg: words[1] & 0xff, bg: (words[1] >>> 8) & 0xff };
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

// The prompt is the last status in red, the cwd's basename white on blue, then
// the $ in bright white. The suite cds, so it tracks where the shell in front
// of it is; the boot cwd is /home.
let cwd = "/home";

function prompt(status = 0) {
    const name = cwd.slice(cwd.lastIndexOf("/") + 1) || "/";
    return `${status ? `[${status}] ` : ""}${name} $`;
}

// Named keys, from the enum in src/kernel/key.h — keep the two in step.
const NAMED = 0x110000;
const KEY = {
    ENTER: NAMED, BACKSPACE: NAMED + 1, TAB: NAMED + 2, ESCAPE: NAMED + 3,
    DELETE: NAMED + 4, UP: NAMED + 6, DOWN: NAMED + 7, LEFT: NAMED + 8,
    RIGHT: NAMED + 9, HOME: NAMED + 10, END: NAMED + 11, PAGE_UP: NAMED + 12,
    PAGE_DOWN: NAMED + 13,
};
const CTRL = 2;

const module = new WebAssembly.Module(readFileSync(file));

function instantiate() {
    instance = new WebAssembly.Instance(module, imports);
    memory = instance.exports.memory;
    view = null;
    u32 = null;
    return instance;
}

instantiate();

const fail = (msg) => {
    console.error(`${basename(file)}: ${msg}`);
    process.exit(1);
};

const names = (list) => list.map((e) => `${e.module ? e.module + "." : ""}${e.name}`).sort();

// A tick, and then whatever the host owes a process. A tier-2 program needs a
// round trip out here per syscall — the kernel cannot call into an instance
// itself — so the driver does the round trips, exactly as web/worker.js does
// with a microtask. Returns the last delay tick reported, so every assertion
// about the timer queue still reads the same.
//
// A delay of 0 means the kernel has ready work and wants running again at once,
// which web/worker.js answers with setTimeout(pump, 0). Looping on it here is
// the same rule: a wake issued while the scheduler sweeps its finished jobs —
// a process reporting its exit status to its parent, say — lands on the ready
// queue after the drain that would have run it.
let ticks = 0;

// The shell is an instance now, and a live one for as long as the system is up,
// so what every assertion below means by "live" is "besides the shell".
// Subtracting it here rather than at sixteen call sites keeps them saying what
// they were written to say.
const others = () => net.proc.live() - 1;

function run(now) {
    let delay = instance.exports.tick(now);
    ticks++;
    while (net.drain() || delay === 0) {
        delay = instance.exports.tick(now);
        ticks++;
    }
    return delay;
}

if (mode === "--kernel") {
    // The import and export surface is the ABI; drift is a bug, and an
    // unexpected import means a libc dependency crept in.
    const want_imports = ["host.fs", "host.fs_sync", "host.log", "host.now", "host.present",
                          "host.svc"];
    const want_exports = ["init", "key", "memory", "ref", "resize", "sys", "sys_async", "tick",
                          "wake"];
    const got_imports = names(WebAssembly.Module.imports(module));
    const got_exports = names(WebAssembly.Module.exports(module));

    if (got_imports.join() !== want_imports.join())
        fail(`imports are [${got_imports}], expected [${want_imports}]`);
    if (got_exports.join() !== want_exports.join())
        fail(`exports are [${got_exports}], expected [${want_exports}]`);

    // The process ABI is a surface of its own (Concept.md §4.3), and the same
    // rule applies to it: drift is a bug. Note what is *not* there — a process
    // imports nothing from the host, and `sys` has no pid argument, which is
    // the whole of "a process cannot issue a syscall on behalf of another".
    // A tier is the binary's claim and nothing more: the surface is the same
    // one at either, which is what lets `exec` pick without userland noticing.
    const want_tier = { "spin.wasm": 3, "tail.wasm": 3 };

    for (const binary of binaries) {
        const bin = new WebAssembly.Module(readFileSync(binary));
        const want_bin_imports = ["env.memory", "kernel.sys", "kernel.sys_async"];
        const want_bin_exports = ["_alloc", "_free", "_resume", "_start"];
        const got_bin_imports = names(WebAssembly.Module.imports(bin));
        const got_bin_exports = names(WebAssembly.Module.exports(bin));

        // A subset, not the whole list: `true` never makes an asynchronous
        // syscall, so it does not import sys_async at all. What is asserted is
        // that nothing *else* is imported — a host import in a binary would
        // mean the process ABI had been gone around.
        for (const name of got_bin_imports)
            if (!want_bin_imports.includes(name))
                fail(`${basename(binary)} imports ${name}, which is not the process ABI`);
        if (!got_bin_imports.includes("env.memory"))
            fail(`${basename(binary)} does not import env.memory`);
        if (got_bin_exports.join() !== want_bin_exports.join())
            fail(`${basename(binary)} exports [${got_bin_exports}], expected ` +
                 `[${want_bin_exports}]`);

        // The memory is imported, so its cap is the kernel's to set: the
        // module declares no maximum of its own to override it.
        const meta = WebAssembly.Module.customSections(bin, "braam");
        if (meta.length !== 1)
            fail(`${basename(binary)} carries ${meta.length} braam sections, expected 1`);
        const m = new Uint32Array(meta[0]);
        if (m[0] !== 0x6d617262 || m[1] !== 5)
            fail(`${basename(binary)}'s metadata is ${m[0].toString(16)}/${m[1]}`);
        const tier = want_tier[basename(binary)] || 2;
        if (m[2] !== tier)
            fail(`${basename(binary)} asks for tier ${m[2]}, expected ${tier}`);
        if (m[5] !== 256)
            fail(`${basename(binary)} asks for ${m[5]} pages, expected 256`);
    }

    instance.exports.init(0);

    if (logged.length !== 1)
        fail(`expected one boot line, got ${logged.length}`);
    if (!logged[0].startsWith("braam "))
        fail(`unexpected boot line: ${logged[0]}`);

    // init spawns the shell and nothing else. The first tick mounts the
    // filesystem, draws the prompt and parks the shell on the keyboard, so
    // there is nothing left pending.
    if (run(0) !== -1)
        fail("the shell did not park on the keyboard");

    // resize() hands back the descriptor; nothing else tells JS where the
    // grid is.
    let addr = instance.exports.resize(60, 16);
    if (addr === 0)
        fail("resize returned no screen descriptor");

    let s = descriptor(addr);
    if (s.magic !== SCREEN_MAGIC)
        fail(`screen magic is ${s.magic.toString(16)}, expected ${SCREEN_MAGIC.toString(16)}`);
    if (s.cols !== 60 || s.rows !== 16)
        fail(`resize(60, 16) gave ${s.cols}x${s.rows}`);

    // A resize repaints everything, and the host ticks to let it out.
    presented.length = 0;
    run(1);
    if (presented.length !== 1 || presented[0].w !== 60 || presented[0].h !== 16)
        fail(`the resize did not repaint the whole screen: ${JSON.stringify(presented)}`);

    // init prints /share/motd before the shell, in green, and the prompt sets
    // its own colour rather than inheriting one. COLOR_GREEN is 2 and
    // COLOR_WHITE|COLOR_BRIGHT is 15, from the enum in src/kernel/screen.h.
    s = descriptor(addr);
    const motd_y = rows(s).findIndex((line) => line.startsWith("braam — a small operating system"));
    if (motd_y < 0)
        fail(`the motd did not print at boot: ${JSON.stringify(rows(s))}`);
    if (cell(s, 0, motd_y).fg !== 2)
        fail(`the motd is colour ${cell(s, 0, motd_y).fg}, expected green`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`the boot prompt is ${row(s, s.cursor_y)}, expected ${prompt()}`);
    // The directory is white on blue, the space beside it is not, and the $ is
    // bright white: three runs, three Sys::Style calls. COLOR_WHITE is 7,
    // COLOR_BLUE is 4 and COLOR_BLACK is 0.
    const dir_end = prompt().length - 2; // "home" is columns 0..3
    if (cell(s, 0, s.cursor_y).fg !== 7 || cell(s, 0, s.cursor_y).bg !== 4)
        fail(`the cwd is ${JSON.stringify(cell(s, 0, s.cursor_y))}, expected white on blue`);
    if (cell(s, dir_end, s.cursor_y).bg !== 0)
        fail(`the space before the $ is on ${cell(s, dir_end, s.cursor_y).bg}, expected black`);
    if (cell(s, dir_end + 1, s.cursor_y).fg !== 15 || cell(s, dir_end + 1, s.cursor_y).bg !== 0)
        fail(`the $ is ${JSON.stringify(cell(s, dir_end + 1, s.cursor_y))}, expected 15 on 0`);

    const type = (text) => {
        for (const ch of text)
            instance.exports.key(ch.codePointAt(0), 0);
    };
    const press = (code, mods = 0) => instance.exports.key(code, mods);
    const submit = (text, now) => {
        type(text);
        press(KEY.ENTER);
        run(now);
        return descriptor(addr);
    };

    // M1's coverage, now supplied by the shell instead of by demo tasks:
    // `sleep` parks on the timer queue, and the delays tick reports are exact
    // because the clock is ours. It exercises argv and `exec` with it, since
    // `sleep` is a binary like everything else.
    type("sleep 30");
    press(KEY.ENTER);
    const delays = [1000, 1010, 1030].map((now) => run(now));
    const want_delays = [30, 20, -1];
    if (delays.join() !== want_delays.join())
        fail(`tick returned [${delays}], expected [${want_delays}]`);

    // M3, first criterion: `echo hello` prints and `help` lists the programs.
    s = submit("echo hello", 1040);
    if (!rows(s).includes("hello"))
        fail(`echo did not print: ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`the prompt after a success is ${row(s, s.cursor_y)}, expected ${prompt()}`);

    // The renderer's selection, read off the real grid rather than a mock of
    // one. A drag names cells in device pixels and the text it gives back is
    // what the page puts on the clipboard; none of it reaches the kernel
    // (Concept.md §3.5), so this is the only check web/render.js gets.
    {
        let ground = []; // the background each cell of a painted row got
        const ctx = {
            measureText: (t) => ({
                width: 8 * [...t].length,
                fontBoundingBoxAscent: 12,
                fontBoundingBoxDescent: 4,
            }),
            fillRect: () => ground.push(ctx.fillStyle),
            fillText: () => {},
        };
        const r = new Renderer({ getContext: () => ctx }, mem, {});
        r.attach(addr);

        const y = rows(s).indexOf("hello");
        const paint = () => {
            ground = [];
            r.present(0, y, s.cols, 1);
            return ground;
        };
        const plain = paint();

        r.select("start", 0, y * r.cellH);
        r.select("move", 4 * r.cellW, y * r.cellH);
        r.select("end", 4 * r.cellW, y * r.cellH);
        if (r.text() !== "hello")
            fail(`the renderer selected ${JSON.stringify(r.text())}, expected "hello"`);

        // And the five cells it named, and no others, are painted reversed.
        const swapped = paint().reduce((n, bg, i) => n + (bg !== plain[i] ? 1 : 0), 0);
        if (swapped !== 5)
            fail(`the selection reversed ${swapped} cells, expected 5`);

        // Dragging the other way names the same cells: the anchor and the head
        // are put in reading order, not in the order they were made.
        r.select("start", 4 * r.cellW, y * r.cellH);
        r.select("end", 0, y * r.cellH);
        if (r.text() !== "hello")
            fail(`a backwards drag selected ${JSON.stringify(r.text())}`);

        // A drag off the edges clamps into the grid, and select-all names the
        // same cells without a drag at all. Neither hands back the blank rows
        // below the last line of output, and neither keeps a trailing blank.
        const screen = rows(s).join("\n").replace(/\n+$/, "");
        r.select("start", -99, -99);
        r.select("end", 1e6, 1e6);
        if (r.text() !== screen)
            fail(`a full-screen drag gave ${JSON.stringify(r.text())}`);
        if (r.text().endsWith("\n") || r.text().includes(" \n"))
            fail("a full-screen drag kept blanks the screen only pads with");

        r.clear();
        r.all();
        if (r.text() !== screen)
            fail(`select-all gave ${JSON.stringify(r.text())}`);

        // A click is not a selection: it clears, so ^C stays an interrupt.
        r.select("start", 8, 8);
        r.select("end", 9, 9);
        if (r.text() !== "" || r.clear())
            fail("a click left a selection behind");
    }

    // The other half of the page's clipboard: a paste is a run of keystrokes
    // and nothing else (Concept.md §3.5). web/keys.js turns the text into them
    // — one Enter for a newline however it is spelled, and no key at all for a
    // control character that no key produces.
    if (pasted("a\r\nb\tc").join() !== [97, KEY.ENTER, 98, KEY.TAB, 99].join())
        fail(`pasted() gave [${pasted("a\r\nb\tc")}]`);

    submit("clear", 1045); // the programs need more than the whole grid
    addr = instance.exports.resize(100, 48);
    if (addr === 0)
        fail("the resize before help failed");
    s = submit("help", 1050);
    for (const name of ["cat", "cd", "chat", "clear", "curl", "date", "df", "echo", "edit",
                        "export", "false", "fg", "grep", "head", "help", "import", "jobs", "kill",
                        "less", "ls", "mkdir", "mount", "pbcopy", "pbpaste", "pwd", "rm", "sleep",
                        "tail", "timeout", "touch", "true", "version", "watch", "wc"])
        if (!rows(s).some((line) => line.startsWith(`  ${name} `)))
            fail(`help did not list ${name}: ${JSON.stringify(rows(s))}`);

    // A pasted line is longer than the 64-slot key ring, so it is fed at the
    // rate the console drains it: key() says whether it took the keystroke, and
    // the rest of the run waits for the tick that empties the ring. This is the
    // loop in web/worker.js, and pushing the run in one go would drop its tail.
    // The screen is wide here, so the echoed line does not wrap.
    {
        const codes = pasted(`echo ${"z".repeat(80)}\r\n`);
        let at = 0, turns = 0;
        while (at < codes.length && turns++ < 40) {
            while (at < codes.length && instance.exports.key(codes[at], 0))
                at++;
            run(1055);
        }
        if (at !== codes.length)
            fail(`the paste stalled after ${at} of ${codes.length} keystrokes`);
        if (turns < 2)
            fail(`the ring took ${codes.length} keystrokes at once, so nothing was paced`);
        s = descriptor(addr);
        if (!rows(s).includes("z".repeat(80)))
            fail(`the pasted line did not run: ${JSON.stringify(rows(s))}`);
        if (row(s, s.cursor_y) !== prompt())
            fail(`the paste left ${row(s, s.cursor_y)}, expected a fresh prompt`);
    }

    addr = instance.exports.resize(60, 16);
    if (addr === 0)
        fail("the resize after help failed");

    // M3, second criterion, first half: a nonzero exit code is observable —
    // the shell carries it in the next prompt.
    s = submit("false", 1060);
    if (!rows(s).includes(prompt(1)))
        fail(`a failing program left ${row(s, s.cursor_y)}, expected ${prompt(1)}`);
    // And it is red, ahead of the cwd on blue and the bright white $: three
    // runs, three Sys::Style calls. COLOR_RED is 1.
    if (cell(s, 0, s.cursor_y).fg !== 1)
        fail(`the status is colour ${cell(s, 0, s.cursor_y).fg}, expected red`);
    if (cell(s, 4, s.cursor_y).bg !== 4)
        fail(`the cwd after a status is on ${cell(s, 4, s.cursor_y).bg}, expected blue`);
    const dollar = prompt(1).length - 1;
    if (cell(s, dollar, s.cursor_y).fg !== 15)
        fail(`the $ after a status is colour ${cell(s, dollar, s.cursor_y).fg}, expected white`);

    s = submit("nosuch", 1070);
    if (!rows(s).some((line) => line.startsWith("braam: nosuch: not found")))
        fail(`an unknown command said nothing: ${JSON.stringify(rows(s))}`);
    if (!rows(s).includes(prompt(127)))
        fail(`an unknown command left ${row(s, s.cursor_y)}, expected ${prompt(127)}`);

    s = submit("true", 1080);
    if (row(s, s.cursor_y) !== prompt())
        fail(`a succeeding program left ${row(s, s.cursor_y)}, expected ${prompt()}`);

    // M3, second criterion, second half: Up recalls history, Home reaches the
    // start of the recalled line, and ^C abandons it.
    press(KEY.UP);
    run(1090);
    s = descriptor(addr);
    if (!row(s, s.cursor_y).endsWith("true"))
        fail(`Up recalled ${row(s, s.cursor_y)}, expected it to end in true`);

    press(KEY.HOME);
    run(1100);
    s = descriptor(addr);
    // The anchor is one past the prompt, whose trailing space row() trims.
    const home_x = prompt().length + 1;
    if (s.cursor_x !== home_x)
        fail(`Home left the cursor at column ${s.cursor_x}, expected ${home_x}`);

    press("c".codePointAt(0), CTRL);
    run(1110);
    s = descriptor(addr);
    if (!rows(s).includes(prompt(130)))
        fail(`^C left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);

    // M2's coverage: at most one present per tick, and between them they cover
    // every cell the editor drew and the cell the cursor left — that one has to
    // repaint or it ghosts.
    //
    // A keystroke is several ticks rather than one now. The editor is a program
    // (Concept.md §4), so repainting a line is a handful of syscalls and each is
    // a step of its own; what M2 asked for was one present per *tick*, and that
    // is still exactly what happens.
    presented.length = 0;
    ticks = 0;
    const x0 = s.cursor_x;
    const y0 = s.cursor_y;
    type("hi");
    run(1120);
    s = descriptor(addr);
    if (s.cursor_x !== x0 + 2)
        fail(`the cursor is at column ${s.cursor_x}, expected ${x0 + 2}`);
    if (!presented.length || presented.length > ticks)
        fail(`${presented.length} presents over ${ticks} ticks`);
    const r = presented.reduce((a, b) => ({
        x: Math.min(a.x, b.x),
        y: Math.min(a.y, b.y),
        w: Math.max(a.x + a.w, b.x + b.w) - Math.min(a.x, b.x),
        h: Math.max(a.y + a.h, b.y + b.h) - Math.min(a.y, b.y),
    }));
    if (r.x > x0 || r.y > y0 || r.x + r.w < s.cursor_x + 1 || r.y + r.h <= y0)
        fail(`presents ${r.x},${r.y} ${r.w}x${r.h} miss ${x0}..${s.cursor_x},${y0}`);

    // M2's second criterion: resize reflows, keeping the rows in use.
    addr = instance.exports.resize(20, 2);
    if (addr === 0)
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

    // M4, first criterion: a pipeline, in the shipping kernel. /bin is the
    // bundle's binaries, and grep filters the listing, both running at once
    // over a bounded pipe. `clear` first, so the rows below are the
    // pipeline's and nothing else's.
    addr = instance.exports.resize(60, 16);
    if (addr === 0)
        fail("the resize before the pipeline failed");
    press("c".codePointAt(0), CTRL); // the "hi" typed above is still pending
    s = submit("clear", 1130);
    s = submit("ls /bin | grep ta", 1140);
    const listed = rows(s).filter((line) => line && !line.includes("$"));
    if (listed.join() !== "tail")
        fail(`ls /bin | grep ta printed ${JSON.stringify(listed)}, expected ["tail"]`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`a pipeline that matched left ${row(s, s.cursor_y)}, expected ${prompt()}`);

    // The status of a pipeline is its last command's: grep reports 1 when
    // nothing matched, and quote removal reaches argv on the way in.
    s = submit("ls /bin | grep zzz", 1150);
    if (!rows(s).includes(prompt(1)))
        fail(`an empty pipeline left ${row(s, s.cursor_y)}, expected ${prompt(1)}`);
    s = submit("echo 'a b' | wc", 1160);
    if (!rows(s).includes("1 2 4"))
        fail(`echo 'a b' | wc printed ${JSON.stringify(rows(s))}, expected 1 2 4`);

    // M5: the shell starts in /home, which is where a redirection lands.
    // `pwd` reads its own cwd through Sys::Chdir now, so this is also the proof
    // that a top-level command inherits the shell's rather than starting at /.
    s = submit("clear", 1165);
    s = submit("pwd", 1166);
    if (!rows(s).includes("/home"))
        fail(`pwd printed ${JSON.stringify(rows(s))}, expected /home`);

    // ...and it follows `cd`, since that is what a process inherits.
    submit("cd /bin", 1167);
    cwd = "/bin";
    s = submit("clear", 1168);
    s = submit("pwd", 1169);
    if (!rows(s).includes("/bin"))
        fail(`pwd after cd printed ${JSON.stringify(rows(s))}, expected /bin`);

    // The prompt names the basename, so the root is the one directory with no
    // name of its own — path_basename answers "/" for it rather than nothing.
    s = submit("cd /", 1169.1);
    cwd = "/";
    if (row(s, s.cursor_y) !== "/ $" || row(s, s.cursor_y) !== prompt())
        fail(`cd / left ${row(s, s.cursor_y)}, expected "/ $"`);

    // A relative path from a program resolves against that inherited cwd, not
    // against the root: `ls .` in /bin has to find the binaries.
    submit("cd /bin", 1169.15);
    cwd = "/bin";
    s = submit("clear", 1169.2);
    s = submit("ls . | grep wc", 1169.4);
    if (!rows(s).includes("wc"))
        fail(`ls . in /bin printed ${JSON.stringify(rows(s))}, expected wc`);
    submit("cd /home", 1169.6);
    cwd = "/home";

    // M5, first criterion, first half: a redirection that really writes, an
    // append that follows it, and a file argument that reads it back.
    submit("echo one > notes", 1170);
    submit("echo two >> notes", 1171);
    s = submit("clear", 1172);
    s = submit("cat notes", 1173);
    const notes = rows(s).filter((line) => line && !line.includes("$"));
    if (notes.join(",") !== "one,two")
        fail(`cat notes printed ${JSON.stringify(notes)}, expected one,two`);

    // A redirection that cannot be opened stops the command before it runs.
    s = submit("echo hi > /bin/wc", 1174);
    if (!rows(s).some((line) => line.startsWith("braam: /bin/wc: ")))
        fail(`a read-only redirection said nothing: ${JSON.stringify(rows(s))}`);
    if (!rows(s).includes(prompt(1)))
        fail(`a refused redirection left ${row(s, s.cursor_y)}, expected ${prompt(1)}`);

    // Coverage that used to live in test_shell, moved here when its programs
    // became binaries: a filter stopping early, a three-stage pipeline, and
    // typing into a running job's stdin with ^D as end of input.
    s = submit("clear", 1174.1);
    submit("mkdir /home/d", 1174.2);
    submit("touch /home/d/a /home/d/b /home/d/c", 1174.3);
    s = submit("clear", 1174.4);
    s = submit("ls /home/d | head -n 2", 1174.5);
    const cut = rows(s).filter((line) => line && !line.includes("$"));
    if (cut.join(",") !== "a,b")
        fail(`head did not stop the producer: ${JSON.stringify(cut)}`);

    s = submit("clear", 1174.6);
    s = submit("ls /home/d | grep b | head -n 1", 1174.7);
    const three = rows(s).filter((line) => line && !line.includes("$"));
    if (three.join(",") !== "b")
        fail(`a three-stage pipeline printed ${JSON.stringify(three)}`);

    // stdin is the pump's other job: what is typed reaches a running program,
    // echoed once by the pump and printed again by cat, and ^D ends the input.
    s = submit("clear", 1174.8);
    type("cat");
    press(KEY.ENTER);
    run(1174.9);
    // Longer than the input pipe has slots, which is the point: the pump sends
    // a cooked line as one chunk, and used to send one per keystroke and drop
    // the rest of the line once the eight slots were full. An applet drained
    // the pipe in the same tick and never showed it; a process reads one
    // syscall at a time and always would.
    type("a longer line than eight");
    press(KEY.ENTER);
    run(1175.0);
    s = descriptor(addr);
    if (rows(s).filter((line) => line === "a longer line than eight").length !== 2)
        fail(`typing into cat printed ${JSON.stringify(rows(s))}, expected it twice`);
    press("d".codePointAt(0), CTRL);
    run(1175.1);
    s = descriptor(addr);
    if (row(s, s.cursor_y) !== prompt())
        fail(`^D did not end cat's input: ${JSON.stringify(rows(s))}`);

    submit("rm -r /home/d", 1175.2);

    // `<` and `2>` both reach the filesystem too.
    s = submit("clear", 1175);
    s = submit("wc < notes", 1176);
    if (!rows(s).some((line) => line.startsWith("2 2 8")))
        fail(`wc < notes printed ${JSON.stringify(rows(s))}, expected 2 2 8`);
    submit("cat nosuchfile 2> err", 1177);
    s = submit("clear", 1178);
    s = submit("cat err", 1179);
    if (!rows(s).some((line) => line.startsWith("cat: nosuchfile: not found")))
        fail(`2> did not capture a diagnostic: ${JSON.stringify(rows(s))}`);

    // The boot archive is mounted read-only as /bin and /share, unpacked from
    // the file tools/pack.py wrote at the end of the build.
    if (bundle) {
        s = submit("clear", 1183);
        s = submit("cat /share/motd", 1184);
        if (!rows(s).some((line) => line.startsWith("braam —")))
            fail(`/share/motd did not read back: ${JSON.stringify(rows(s))}`);
        s = submit("clear", 1185);
        s = submit("ls /share", 1186);
        if (!rows(s).includes("doc/"))
            fail(`/share did not list its directories: ${JSON.stringify(rows(s))}`);
    }

    // M5, second criterion: df reports quota, usage and durability.
    s = submit("clear", 1180);
    s = submit("df", 1181);
    const df = rows(s);
    if (!df.some((line) => line.startsWith("backend   opfs")))
        fail(`df did not name the backend: ${JSON.stringify(df)}`);
    if (!df.some((line) => line.startsWith("mode      best-effort")))
        fail(`df did not report the mode: ${JSON.stringify(df)}`);
    if (!df.some((line) => line.startsWith(`quota     ${store.quota} bytes`)))
        fail(`df did not report the quota: ${JSON.stringify(df)}`);
    if (!df.some((line) => /^used      \d+ bytes$/.test(line)))
        fail(`df did not report the usage: ${JSON.stringify(df)}`);

    // M4, second criterion: ^C interrupts a running pipeline and the prompt
    // comes back. tick's return value is what proves the sleep really went.
    type("sleep 5000");
    press(KEY.ENTER);
    if (run(1190) !== 5000)
        fail("the pipeline did not park on the timer");
    press("c".codePointAt(0), CTRL);
    if (run(1200) !== -1)
        fail("^C left the pipeline's timer armed");
    s = descriptor(addr);
    if (!rows(s).includes(prompt(130)))
        fail(`^C on a pipeline left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);

    // The keyboard came back: the pump was its only receiver while the job
    // ran, and the shell has to be able to read it again afterwards.
    s = submit("echo back", 1210);
    if (!rows(s).includes("back"))
        fail(`the shell lost the keyboard after ^C: ${JSON.stringify(rows(s))}`);

    // An asynchronous reply really does park the task: hold the reply back and
    // the shell has nothing pending until it lands, which is the path a browser
    // always takes and this harness otherwise short-circuits.
    store.defer = true;
    type("ls /home");
    press(KEY.ENTER);
    if (run(1220) !== -1)
        fail("a deferred storage reply left something scheduled");
    if (store.held.length === 0)
        fail("ls issued no storage request");
    store.defer = false;
    while (store.held.length)
        instance.exports.wake(store.held.shift(), 0, 0);
    run(1230);
    s = descriptor(addr);
    if (!rows(s).includes("notes"))
        fail(`the deferred listing never arrived: ${JSON.stringify(rows(s))}`);

    // M5, first criterion, second half: throw the instance away and build a
    // new one against the same store. That is what a reload does, and the file
    // written above has to still be there afterwards.
    const before = logged.length;
    store.reopen();
    instantiate();
    instance.exports.init(0);
    if (logged.length !== before + 1)
        fail("the second boot did not log its banner");

    addr = instance.exports.resize(60, 16);
    if (addr === 0)
        fail("the reloaded kernel has no screen");
    run(2000);
    submit("clear", 2005); // the second boot banner is still on the grid
    s = submit("cat notes", 2010);
    const survived = rows(s).filter((line) => line && !line.includes("$"));
    if (survived.join(",") !== "one,two")
        fail(`the file did not survive the reload: ${JSON.stringify(survived)}`);

    // M5, third criterion: with no OPFS the system still boots, on MemFs, and
    // says so rather than letting the user find out by losing a file.
    const archive = store.bundle;
    store.reset();
    store.bundle = archive; // served beside kernel.wasm; a reload still finds it
    store.opfs = false;
    store.sync = false;
    instantiate();
    instance.exports.init(0);
    addr = instance.exports.resize(60, 16);
    run(3000);
    s = descriptor(addr);
    if (!rows(s).some((line) => line.startsWith("braam: no OPFS")))
        fail(`booting without OPFS said nothing: ${JSON.stringify(rows(s))}`);

    s = submit("echo volatile > gone", 3010);
    s = submit("clear", 3011);
    s = submit("cat gone", 3012);
    if (!rows(s).includes("volatile"))
        fail(`the memory fallback is not writable: ${JSON.stringify(rows(s))}`);

    // M6, first criterion: curl fetches a URL and prints the body. -i puts the
    // status and the headers in front of it.
    s = submit("clear", 3020);
    s = submit("curl /hello.txt", 3021);
    if (!rows(s).includes("hi there"))
        fail(`curl printed nothing: ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3022);
    s = submit("curl -i /hello.txt", 3023);
    if (!rows(s).includes("HTTP 200"))
        fail(`curl -i did not print the status: ${JSON.stringify(rows(s))}`);
    if (!rows(s).some((line) => line.startsWith("content-type: text/plain")))
        fail(`curl -i did not print the headers: ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3024);
    s = submit("curl /nosuch", 3025);
    if (!rows(s).some((line) => line.startsWith("curl: /nosuch: not found")))
        fail(`a failed fetch said nothing: ${JSON.stringify(rows(s))}`);

    // A fetched body reaches a pipe like any other output.
    s = submit("clear", 3026);
    s = submit("curl /hello.txt | wc", 3027);
    if (!rows(s).some((line) => line.startsWith("1 2 9")))
        fail(`curl into a pipe printed ${JSON.stringify(rows(s))}, expected 1 2 9`);

    // The two ways a fetch fails in a browser, which look identical to fetch
    // itself: an origin the server will not allow, and nothing answering at
    // all. Each gets the hint that fits, and neither gets the other's.
    net.routes.set("https://denied.example", { fail: E.PERM });
    net.routes.set("https://dead.example", { fail: E.IO });

    s = submit("clear", 3028);
    s = submit("curl https://denied.example", 3029);
    if (!rows(s).some((line) => line.includes("cross-origin")))
        fail(`a refused origin said nothing about CORS: ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3031);
    s = submit("curl https://dead.example", 3033);
    if (!rows(s).some((line) => line.startsWith("curl: no answer")))
        fail(`a dead network said nothing: ${JSON.stringify(rows(s))}`);
    if (rows(s).some((line) => line.includes("cross-origin")))
        fail(`a dead network was blamed on CORS: ${JSON.stringify(rows(s))}`);

    // M6, second criterion: a chat client over a WebSocket. The fake loops a
    // lone socket back to itself, so one client is a whole conversation; the
    // receiver is a job of its own, which is what makes the reply arrive while
    // the program is parked on the keyboard.
    s = submit("clear", 3030);
    // What arrives is written to stdout by the receiver task, so it redirects
    // like anything else — which it could not when the receiver was a job
    // outside the program, writing to the screen because the pipe it would
    // have used might already have been freed. First, while this is the only
    // socket: the fake loops a message back to a sender with no peer.
    type("chat ws://loop me > /home/chat.log");
    press(KEY.ENTER);
    run(3030.1);
    s = submit("hi there", 3030.2);
    press("c".codePointAt(0), CTRL);
    run(3030.3);
    s = submit("clear", 3030.4);
    s = submit("cat /home/chat.log", 3030.5);
    if (!rows(s).includes("me: hi there"))
        fail(`chat > log captured ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3030.6);
    type("chat ws://loop me");
    press(KEY.ENTER);
    run(3031);
    if (net.sockets.length !== 2)
        fail(`chat opened ${net.sockets.length} sockets, expected 2`);
    s = submit("hello", 3032);
    if (!rows(s).includes("me: hello"))
        fail(`the chat message did not come back: ${JSON.stringify(rows(s))}`);

    // ^C leaves the socket dropped and the prompt back. The receiver is a
    // second task inside the process now, so it goes when the instance does.
    press("c".codePointAt(0), CTRL);
    if (run(3033) !== -1)
        fail("^C left the chat receiver scheduled");
    s = descriptor(addr);
    if (!rows(s).includes(prompt(130)))
        fail(`^C on chat left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);
    if (!net.sockets[1].closed)
        fail("chat did not drop its socket on the way out");

    // A descriptor closed under a parked read, which is the one sequence a
    // program can arrange: chat's receiver is parked on the socket while the
    // root task reads what is typed, and end of input closes that socket. The
    // close is served while the read is parked, because the root task waits for
    // its reply. What comes back is chat's own status, not a crash.
    s = submit("clear", 3034);
    type("chat ws://loop me");
    press(KEY.ENTER);
    run(3035);
    if (net.sockets.length !== 3)
        fail(`chat opened ${net.sockets.length} sockets, expected 3`);
    s = submit("still here", 3036); // the receiver parks again after this
    press("d".codePointAt(0), CTRL);
    if (run(3037) !== -1)
        fail("end of input left the chat receiver scheduled");
    s = descriptor(addr);
    if (!net.sockets[2].closed)
        fail("chat did not close its socket at end of input");
    if (row(s, s.cursor_y) !== prompt())
        fail(`^D on chat left ${JSON.stringify(rows(s))}, expected a bare prompt`);

    // M6, third criterion: /mnt/import takes what the picker hands over, and
    // export sends a file back out through the browser.
    net.reset();
    s = submit("clear", 3040);
    s = submit("import", 3041);
    if (!rows(s).includes("/mnt/import/notes.txt"))
        fail(`import named nothing: ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3042);
    s = submit("cat /mnt/import/notes.txt", 3043);
    if (!rows(s).includes("picked"))
        fail(`the imported file did not read back: ${JSON.stringify(rows(s))}`);

    submit("export /mnt/import/notes.txt", 3044);
    if (net.saved.length !== 1 || net.saved[0].name !== "notes.txt")
        fail(`export saved ${JSON.stringify(net.saved.map((f) => f.name))}`);
    if (new TextDecoder().decode(net.saved[0].bytes) !== "picked\n")
        fail(`export saved the wrong bytes: ${JSON.stringify(net.saved[0].bytes)}`);

    // The clipboard, and the wall clock the kernel's monotonic one cannot give.
    submit("echo copied | pbcopy", 3050);
    if (net.clipboard !== "copied\n")
        fail(`pbcopy left ${JSON.stringify(net.clipboard)} on the clipboard`);
    s = submit("clear", 3051);
    s = submit("pbpaste", 3052);
    if (!rows(s).includes("copied"))
        fail(`pbpaste printed nothing: ${JSON.stringify(rows(s))}`);

    // A browser that will not read the clipboard outside a user gesture — which
    // a command can never be. pbpaste asks for the one gesture that needs no
    // permission and parks until it happens.
    net.clipDenied = true;
    s = submit("clear", 3055);
    type("pbpaste");
    press(KEY.ENTER);
    if (run(3056) !== -1)
        fail("pbpaste did not park on the paste");
    s = descriptor(addr);
    if (!rows(s).some((line) => line.startsWith("pbpaste: press ")))
        fail(`pbpaste did not ask for a gesture: ${JSON.stringify(rows(s))}`);
    if (!net.paste("pasted by hand"))
        fail("pbpaste was not waiting for a paste");
    run(3057);
    s = descriptor(addr);
    if (!rows(s).includes("pasted by hand"))
        fail(`the paste never arrived: ${JSON.stringify(rows(s))}`);

    // ^C while it waits gets the prompt back, and leaves the arming behind it
    // to be reaped when the paste finally lands.
    type("pbpaste");
    press(KEY.ENTER);
    run(3058);
    press("c".codePointAt(0), CTRL);
    if (run(3059) !== -1)
        fail("^C left the paste wait scheduled");
    s = descriptor(addr);
    if (!rows(s).includes(prompt(130)))
        fail(`^C on pbpaste left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);
    net.paste("too late");
    run(3060);
    net.clipDenied = false;

    s = submit("clear", 3053);
    s = submit("date -u", 3054);
    if (!rows(s).some((line) => /^\w\w\w \w\w\w \d\d \d\d:\d\d:\d\d \+0000 \d{4}$/.test(line)))
        fail(`date printed ${JSON.stringify(rows(s))}`);

    // M7, first criterion: a full-screen editor opens a file, edits it, saves
    // it, and gives the shell's screen back on the way out.
    s = submit("clear", 3070);
    s = submit("edit /home/m7.txt", 3071);
    if (!rows(s).some((line) => line.startsWith(" /home/m7.txt")))
        fail(`edit drew no status line: ${JSON.stringify(rows(s))}`);
    if (row(s, 0) !== "")
        fail(`edit did not take the screen: ${JSON.stringify(rows(s))}`);

    type("hello");
    press(KEY.ENTER);
    type("editor");
    run(3072);
    s = descriptor(addr);
    if (row(s, 0) !== "hello" || row(s, 1) !== "editor")
        fail(`typing did not reach the buffer: ${JSON.stringify(rows(s))}`);
    if (!rows(s).some((line) => line.startsWith(" /home/m7.txt *")))
        fail(`the modified flag never showed: ${JSON.stringify(rows(s))}`);

    // Editing what is already there: back to the start of the line, and a
    // character in the middle of it.
    press(KEY.HOME);
    press(KEY.UP);
    press(KEY.RIGHT);
    type("X");
    press("s".codePointAt(0), CTRL); // save
    run(3073);
    s = descriptor(addr);
    if (row(s, 0) !== "hXello")
        fail(`the edit did not land: ${JSON.stringify(rows(s))}`);
    if (rows(s).some((line) => line.startsWith(" /home/m7.txt *")))
        fail(`the buffer is still modified after a save: ${JSON.stringify(rows(s))}`);

    press("q".codePointAt(0), CTRL); // quit
    run(3074);
    s = descriptor(addr);
    if (row(s, s.cursor_y) !== prompt())
        fail(`edit did not give the screen back: ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3075);
    s = submit("cat /home/m7.txt", 3076);
    const saved = rows(s).filter((line) => line && !line.includes("$"));
    if (saved.join(",") !== "hXello,editor")
        fail(`the editor saved ${JSON.stringify(saved)}`);

    // A pager over a pipe: it reads its input to the end, then takes the keys.
    // The bundled README is the input because it is longer than the pane, which
    // is what makes PgDn mean anything.
    s = submit("clear", 3077);
    s = submit("cat /share/doc/README | less", 3078);
    if (!rows(s).some((line) => line.startsWith(" stdin ")))
        fail(`less drew no status line: ${JSON.stringify(rows(s))}`);
    if (row(s, 0) !== "This tree is /share, and it is read-only.")
        fail(`less painted ${JSON.stringify(rows(s))}`);
    press(KEY.PAGE_DOWN);
    run(3079);
    s = descriptor(addr);
    if (row(s, 0) === "This tree is /share, and it is read-only.")
        fail("PgDn did not scroll the pager");
    press("q".codePointAt(0));
    run(3080);
    s = descriptor(addr);
    if (row(s, s.cursor_y) !== prompt())
        fail(`less did not give the screen back: ${JSON.stringify(rows(s))}`);

    // Two claimants at once, which spawning made natural and a pipeline of two
    // pagers reaches from the prompt. The terminal has one holder: whichever
    // stage asks second is refused, rather than snapshotting the blanked grid
    // the first is painting and handing that back as the shell's screen.
    s = submit("clear", 3061);
    s = submit("less /share/doc/README | less", 3062);
    if (!rows(s).some((line) => line.includes("q quits")))
        fail(`neither pager took the screen: ${JSON.stringify(rows(s))}`);
    press("q".codePointAt(0));
    run(3063);
    s = descriptor(addr);
    if (!rows(s).some((line) => line === "less: no keyboard"))
        fail(`the second claimant was not refused: ${JSON.stringify(rows(s))}`);
    if (!row(s, s.cursor_y).endsWith("$"))
        fail(`the screen did not come back: ${JSON.stringify(rows(s))}`);

    // M7, second criterion: a job is backgrounded and listed, and its finish
    // is announced at the next prompt.
    s = submit("clear", 3081);
    s = submit("sleep 5000 &", 3082);
    if (!rows(s).some((line) => line.startsWith("[1] ")))
        fail(`& said nothing: ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`& did not come back to a prompt: ${JSON.stringify(rows(s))}`);

    s = submit("jobs", 3083);
    if (!rows(s).some((line) => line.startsWith("[1]+ running sleep 5000 &")))
        fail(`jobs listed nothing: ${JSON.stringify(rows(s))}`);

    // There is no /proc/jobs any more: the table is the shell's own memory now
    // that the shell is a process, and no syscall shows one process another's.
    // The job's stages are still scheduler tasks, so /proc has a file each.
    s = submit("clear", 3084);
    s = submit("cat /proc/jobs", 3085);
    if (!rows(s).some((line) => line.includes("not found")))
        fail(`/proc/jobs still exists: ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3086);
    s = submit("cat /proc/meminfo", 3087);
    if (!rows(s).some((line) => line.startsWith("reserved ")))
        fail(`/proc/meminfo said nothing: ${JSON.stringify(rows(s))}`);

    // fg brings it back to the foreground and waits: the shell does not reach a
    // prompt until the job is done, and ^C reaches what fg adopted through fg's
    // own destructor. This was test_jobs's, until backgrounding a job that
    // stays running came to need a program the in-wasm tests cannot step.
    s = submit("clear", 3088);
    type("fg");
    press(KEY.ENTER);
    run(3089);
    s = descriptor(addr);
    if (row(s, s.cursor_y) === prompt())
        fail(`fg came straight back to a prompt: ${JSON.stringify(rows(s))}`);
    press("c".codePointAt(0), CTRL);
    run(3090);
    s = descriptor(addr);
    if (!rows(s).includes(prompt(130)))
        fail(`^C during fg left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);
    s = submit("jobs", 3091);
    if (rows(s).some((line) => line.includes("running")))
        fail(`^C during fg left the job running: ${JSON.stringify(rows(s))}`);
    s = submit("clear", 3091.4);
    s = submit("jobs", 3091.5);
    if (rows(s).some((line) => line.startsWith("[1]")))
        fail(`the killed job was never dropped: ${JSON.stringify(rows(s))}`);

    // And a job cancelled outright, which is the other half: kill %n reaches
    // every stage, and the shell stays where it was.
    s = submit("clear", 3092);
    s = submit("sleep 5000 &", 3093);
    s = submit("kill %2", 3094);
    s = submit("jobs", 3095);
    if (rows(s).some((line) => line.includes("running")))
        fail(`kill %2 left the job running: ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3096);
    s = submit("sleep 5000 &", 3097);

    // The timer finally fires, and the job is reported and dropped.
    run(9000);
    s = submit("", 9001);
    if (!rows(s).some((line) => /^\[\d+\] done/.test(line)))
        fail(`the finished job was never announced: ${JSON.stringify(rows(s))}`);
    s = submit("clear", 9002);
    s = submit("jobs", 9003);
    if (rows(s).some((line) => /^\[\d+\]/.test(line)))
        fail(`the finished job is still listed: ${JSON.stringify(rows(s))}`);

    // M8. Everything above this line already ran a tier-2 program without
    // saying so: `wc` is a binary in /bin now, and `echo 'a b' | wc`,
    // `wc < notes` and `curl /hello.txt | wc` are the assertions M4, M5 and M6
    // wrote against the applet, unchanged. That is the third criterion.

    // M8, first criterion: a program with a memory of its own, and a cap the
    // kernel set rather than the binary. hog takes everything it can and then
    // asks memory.grow for one page more.
    s = submit("clear", 9010);
    s = submit("hog", 9011);
    const hogged = rows(s).find((line) => line.startsWith("hog: pid "));
    if (!hogged)
        fail(`hog said nothing: ${JSON.stringify(rows(s))}`);
    if (!/^hog: pid \d+, took 1[0-9] MiB, memory is 256 pages$/.test(hogged))
        fail(`hog reported ${JSON.stringify(hogged)}`);
    if (!rows(s).includes("hog: memory.grow refused past the cap"))
        fail(`memory.grow was not capped: ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`hog exited ${row(s, s.cursor_y)}, expected a bare prompt`);

    // M8, second criterion: the pid a process sees is the one the host bound
    // into its import closure. Two runs are two processes, and neither can
    // name the other — the syscall has no argument for it (asserted against
    // the module's imports above).
    s = submit("clear", 9012);
    s = submit("hog", 9013);
    const again = rows(s).find((line) => line.startsWith("hog: pid "));
    if (!again || again === hogged)
        fail(`a second process reported the same pid: ${JSON.stringify(again)}`);

    // Two instances alive at once, each with sixteen megabytes that are
    // nobody else's, feeding one another through a kernel pipe.
    net.peak = 0;
    s = submit("clear", 9014);
    s = submit("tail -n 1 /share/motd | wc", 9015);
    if (!rows(s).some((line) => /^1 \d+ \d+$/.test(line)))
        fail(`a tier-2 pipeline printed ${JSON.stringify(rows(s))}`);
    if (net.peak < 2)
        fail(`the pipeline peaked at ${net.peak} instances, expected 2`);

    // A process is an ordinary scheduler job: /proc lists it under argv[0],
    // and ^C reaches it through the pipe it is parked on. `wc` with no
    // argument reads its stdin, which nothing is going to write.
    type("wc");
    press(KEY.ENTER);
    if (run(9020) !== -1)
        fail("wc did not park on its stdin");
    s = submit("clear", 9021); // the ^C below needs the pipeline still running
    press("c".codePointAt(0), CTRL);
    if (run(9022) !== -1)
        fail("^C left the process scheduled");
    s = descriptor(addr);
    if (!rows(s).includes(prompt(130)))
        fail(`^C on a process left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);
    if (others() !== 0)
        fail(`${others()} instances outlived their processes`);

    // A file that is not a program is refused before anything runs, and says
    // so differently from a name that is not there at all.
    s = submit("clear", 9030);
    s = submit("/share/motd", 9031);
    if (!rows(s).some((line) => line.startsWith("braam: /share/motd: not executable")))
        fail(`a non-binary was not refused: ${JSON.stringify(rows(s))}`);
    if (!rows(s).includes(prompt(126)))
        fail(`a non-binary left ${row(s, s.cursor_y)}, expected ${prompt(126)}`);

    // help lists what is runnable, whatever tier it runs at.
    addr = instance.exports.resize(100, 48);
    s = submit("clear", 9040);
    s = submit("help", 9041);
    for (const name of ["echo", "hog", "sleep", "spin", "tail", "wc"])
        if (!rows(s).some((line) => line.startsWith(`  ${name} `)))
            fail(`help did not list ${name}`);
    addr = instance.exports.resize(60, 16);

    // M9. `tail` is a tier-3 binary, so the pipeline above already ran a
    // process in a worker of its own, feeding one in this one — that assertion
    // is M8's, unedited, and this is the only line that notices.
    if (!net.bound.length)
        fail("nothing ran at tier 3");

    // A tier-3 program end to end: getpid answered inside its own worker, a
    // write relayed back through the kernel, an exit status carried on the
    // step that reported it.
    net.terminated.length = 0;
    s = submit("clear", 9050);
    s = submit("spin 1", 9051);
    if (!rows(s).some((line) => /^spin: pid \d+, spinning briefly$/.test(line)))
        fail(`a tier-3 program printed ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`spin 1 exited ${row(s, s.cursor_y)}, expected a bare prompt`);
    if (net.terminated.length !== 0)
        fail("a process that exited had its worker terminated rather than pooled");
    if (net.proc.pooled() !== 2)
        fail(`the pool holds ${net.proc.pooled()} workers, expected the two hired at`
             + " boot, spin's own having been put back rather than terminated");

    // M9, first criterion: a program that does not come back. The fake link
    // leaves its step undelivered, which is all the kernel ever sees of a real
    // loop — there is no reply, no timer, and nothing to cancel but the proxy.
    net.hold();
    type("spin");
    press(KEY.ENTER);
    if (run(9060) !== -1)
        fail("a spinning process left the kernel with work to do");
    if (others() !== 1)
        fail("spin did not reach an instance");

    s = submit("clear", 9061); // the ^C below needs the process still running
    press("c".codePointAt(0), CTRL);
    if (run(9062) !== -1)
        fail("^C left the process scheduled");
    s = descriptor(addr);
    if (!rows(s).includes(prompt(130)))
        fail(`^C on a spinning process left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);
    if (net.terminated.length !== 1)
        fail(`${net.terminated.length} workers were terminated, expected 1`);
    if (others() !== 0)
        fail(`${others()} instances outlived their processes`);

    // The reply the terminated worker will never send, arriving anyway: it is
    // dropped, and the shell is none the wiser.
    net.release();
    run(9063);
    s = submit("echo after", 9064);
    if (!rows(s).includes("after"))
        fail(`the shell did not survive a killed process: ${JSON.stringify(rows(s))}`);

    // M9, second criterion: the shell keeps working while one is spinning.
    // Backgrounded, since a foreground job is waited for at every tier — what
    // is being asserted is that the kernel is free, not that the shell is rude.
    net.hold();
    s = submit("clear", 9070);
    s = submit("spin &", 9071);
    const announced = rows(s).find((line) => /^\[\d+\] \d+$/.test(line));
    if (!announced)
        fail(`a backgrounded process did not announce itself: ${JSON.stringify(rows(s))}`);
    const job = announced.slice(1, announced.indexOf("]"));

    s = submit("echo alive", 9072);
    if (!rows(s).includes("alive"))
        fail(`the shell stalled behind a spinning process: ${JSON.stringify(rows(s))}`);
    s = submit("jobs", 9073);
    if (!rows(s).some((line) => line.startsWith(`[${job}]`) && line.includes(" running spin")))
        fail(`jobs did not list the spinning process: ${JSON.stringify(rows(s))}`);

    net.terminated.length = 0;
    s = submit(`kill %${job}`, 9074);
    if (net.terminated.length !== 1)
        fail(`kill terminated ${net.terminated.length} workers, expected 1`);
    if (others() !== 0)
        fail(`${others()} instances outlived kill %1`);
    net.release();

    // ---------------------------------------------------------- processes
    //
    // A program that starts a program. Everything below runs through Sys::Spawn,
    // Sys::Wait and Sys::Kill, and none of it could be a builtin.
    // The ordinary path: the child runs, writes to the stdio it shared with its
    // parent, and its status is what `timeout` reports.
    s = submit("clear", 9100);
    s = submit("timeout 10000 echo child", 9101);
    if (!rows(s).includes("child"))
        fail(`a spawned child printed ${JSON.stringify(rows(s))}, expected child`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`timeout over a fast child left ${row(s, s.cursor_y)}, expected a bare prompt`);
    if (others() !== 0)
        fail(`${others()} instances outlived timeout`);

    // The kill path: the child outlasts the delay, so the alarm task kills it
    // and 124 says which of the two ended it. The clock has to be moved past
    // the delay by hand, exactly as `sleep 30` above needs it.
    s = submit("clear", 9110);
    type("timeout 20 sleep 10000");
    press(KEY.ENTER);
    run(9111);
    if (others() !== 2)
        fail(`timeout over a slow child left ${others()} instances, expected 2`);
    run(9200); // past the delay: the alarm fires here
    s = descriptor(addr);
    if (!rows(s).includes(prompt(124)))
        fail(`timeout did not fire: ${JSON.stringify(rows(s))}`);
    if (others() !== 0)
        fail(`${others()} instances outlived a fired timeout`);

    // The ownership chain, under the load it exists for: the child writes into
    // a pipe the *shell's* Job owns, and that block has to outlive both the
    // stage and every syscall server still parked on it.
    s = submit("clear", 9115);
    s = submit("timeout 10000 echo one two | wc", 9116);
    if (!rows(s).some((line) => line.trim() === "1 2 8"))
        fail(`a supervised child in a pipeline printed ${JSON.stringify(rows(s))}`);
    if (others() !== 0)
        fail(`${others()} instances outlived a supervised pipeline`);

    // A child's exit status reaches the parent, and the parent's reaches the
    // shell — two Waits deep, since `false` is a process of its own.
    s = submit("clear", 9120);
    s = submit("timeout 10000 false", 9121);
    if (!rows(s).includes(prompt(1)))
        fail(`a child's status did not reach the shell: ${JSON.stringify(rows(s))}`);

    // A name that is not a command is the parent's diagnostic, not a crash.
    s = submit("clear", 9130);
    s = submit("timeout 10000 nosuchthing", 9131);
    if (!rows(s).includes(prompt(127)))
        fail(`spawning a missing command gave ${JSON.stringify(rows(s))}, expected 127`);

    // A builtin is the shell's own state and cannot be spawned into a process.
    s = submit("clear", 9140);
    s = submit("timeout 10000 cd", 9141);
    if (!rows(s).some((line) => line.startsWith("timeout: cd:")))
        fail(`spawning a builtin gave ${JSON.stringify(rows(s))}, expected a refusal`);

    // The child inherits the parent's cwd, which the parent inherited from the
    // shell — the whole chain, asserted in one line.
    submit("cd /bin", 9150);
    cwd = "/bin";
    s = submit("clear", 9151);
    s = submit("timeout 10000 pwd", 9152);
    if (!rows(s).includes("/bin"))
        fail(`a child's inherited cwd printed ${JSON.stringify(rows(s))}, expected /bin`);
    submit("cd /home", 9153);
    cwd = "/home";

    // A pipe between a process and its child. `watch` moves the write end into
    // the child, so the child exiting is what closes the channel and gives the
    // read its end of input — nothing else ends that loop.
    s = submit("clear", 9145);
    type("watch -n 100000 echo tick");
    press(KEY.ENTER);
    run(9146);
    if (!rows(descriptor(addr)).includes("tick"))
        fail(`watch printed ${JSON.stringify(rows(descriptor(addr)))}, expected tick`);
    if (others() !== 1)
        fail(`${others()} instances between rounds, expected watch alone`);
    press("c".codePointAt(0), CTRL);
    if (run(9147) !== -1)
        fail("^C on watch left the scheduler with work to do");
    s = descriptor(addr);
    if (!rows(s).includes(prompt(130)))
        fail(`^C on watch left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);
    if (others() !== 0)
        fail(`${others()} instances outlived watch`);

    // A child at tier 3 gets the worker kill through its parent, which is the
    // proof that a spawned process is an ordinary scheduler job.
    //
    // The pool has to be warm before the link is held: `spin` with no argument
    // never comes back, and a tier-3 binary that cannot be given a worker runs
    // at tier 2 instead (§4's fallback) — where the driver would step that loop
    // on its own stack and never return.
    s = submit("spin 1", 9155);
    if (net.proc.pooled() !== 1)
        fail(`the pool holds ${net.proc.pooled()} workers, expected one to hold spin`);

    net.terminated.length = 0;
    net.hold();
    s = submit("clear", 9160);
    type("timeout 20 spin");
    press(KEY.ENTER);
    run(9161);
    if (others() !== 2)
        fail(`a supervised tier-3 child left ${others()} instances, expected 2`);
    run(9200); // past the delay, so the kill is the alarm's and not ^C's
    net.release();
    run(9201);
    if (net.terminated.length !== 1)
        fail(`timeout over a tier-3 child terminated ${net.terminated.length} workers, expected 1`);
    if (others() !== 0)
        fail(`${others()} instances outlived a killed tier-3 child`);

    // ^C reaches a whole chain: the shell cancels the stage, the stage's End
    // cancels the child, and neither is left behind.
    s = submit("spin 1", 9205); // warm the pool again, for the reason above
    net.hold();
    s = submit("clear", 9210);
    type("timeout 100000 spin");
    press(KEY.ENTER);
    run(9211);
    if (others() !== 2)
        fail(`a supervised child left ${others()} instances, expected 2`);
    press("c".codePointAt(0), CTRL);
    if (run(9212) !== -1)
        fail("^C on a spawned pair left the scheduler with work to do");
    net.release();
    run(9213);
    if (others() !== 0)
        fail(`${others()} instances outlived ^C on a parent and its child`);
    s = submit("echo after", 9214);
    if (!rows(s).includes("after"))
        fail(`the shell did not survive ^C on a spawned pair: ${JSON.stringify(rows(s))}`);

    // A worker that is made and never loads its script. The kernel learns at
    // the first step, so the process reads as a crash — and the tier is given
    // up there, because whether it works is a question about procworker.js and
    // not about what is running. Nothing else can ask it: `procs` is never
    // empty once a tier-3 process is permanent, which is what the old latch
    // waited for and would never have seen again.
    net.broken = true;
    net.proc.dropWorkers();
    s = submit("clear", 9076);
    s = submit("spin 1", 9077);
    if (!rows(s).some((line) => line.startsWith("braam: /bin/spin: crashed")))
        fail(`a worker that never loaded printed ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== prompt(132))
        fail(`a crashed process left ${row(s, s.cursor_y)}, expected ${prompt(132)}`);
    if (net.proc.stats().workers)
        fail("a worker that never loaded left tier 3 on");

    const made = net.links.length;
    s = submit("clear", 9078);
    s = submit("spin 1", 9079);
    if (!rows(s).some((line) => /^spin: pid \d+, spinning briefly$/.test(line)))
        fail(`the command after a broken worker printed ${JSON.stringify(rows(s))}`);
    if (net.links.length !== made)
        fail(`${net.links.length - made} workers were hired after the tier was given up`);
    net.broken = false;

    // Where a worker cannot be made at all, a binary asking for tier 3 runs at
    // tier 2 — the same program, the same output, one isolation weaker (§4).
    net.workers = false;
    net.proc.dropWorkers();
    net.bound.length = 0;
    s = submit("clear", 9080);
    s = submit("spin 1", 9081);
    if (!rows(s).some((line) => /^spin: pid \d+, spinning briefly$/.test(line)))
        fail(`the tier-2 fallback printed ${JSON.stringify(rows(s))}`);
    if (net.bound.length !== 0)
        fail("the fallback still bound a worker");
    if (row(s, s.cursor_y) !== prompt())
        fail(`the fallback exited ${row(s, s.cursor_y)}, expected a bare prompt`);

    // The builtins are the shell's own state, so they are not files: `cd` is
    // not in /bin and never resolves through it, and `help` prints them ahead
    // of everything else, with the usage line the table carries.
    addr = instance.exports.resize(100, 48);
    s = submit("clear", 9090);
    s = submit("help", 9091);
    const helped = rows(s).filter((line) => line.startsWith("  "));
    for (const [i, name] of ["cd", "exit", "fg", "help", "jobs", "kill"].entries())
        if (!helped[i] || !helped[i].startsWith(`  ${name} `))
            fail(`help listed ${JSON.stringify(helped[i])} at ${i}, expected ${name}`);
    if (!helped.some((line) => line.startsWith("  wc ") && line.includes("count lines")))
        fail(`help did not carry /share/help's usage for wc: ${JSON.stringify(helped)}`);
    addr = instance.exports.resize(60, 16);

    s = submit("clear", 9092);
    s = submit("ls /bin", 9093);
    if (rows(s).includes("cd"))
        fail("cd is a builtin and must not be a file in /bin");

    // A builtin is an ordinary pipeline stage, which is why `fg` can claim the
    // pump of the pipeline it is running in — so it pipes and redirects too.
    s = submit("clear", 9094);
    s = submit("jobs > /home/j", 9095);
    s = submit("cat /home/j", 9096);
    if (row(s, s.cursor_y) !== prompt())
        fail(`a redirected builtin left ${row(s, s.cursor_y)}, expected a bare prompt`);

    // /bin/sh: the shell as an ordinary program, running as a child of the
    // resident one. Everything below happens over the §4.3 syscall table —
    // Cursor for the prompt, KeyClaim for the keys, Pipe/Spawn/Wait for the
    // pipeline, Chdir for `cd` — and nothing in it is kernel code.
    s = submit("clear", 9200);
    s = submit("sh", 9201);
    // Two prompts on one screen: the resident shell's, with `sh` typed at it,
    // and the one the child drew for itself.
    // The child inherits the cwd, so both prompts read the same.
    if (!rows(s).includes(`${prompt()} sh`) || row(s, s.cursor_y) !== prompt())
        fail(`sh drew ${JSON.stringify(rows(s))}, expected its own prompt under "${prompt()} sh"`);

    // Its line editor: typing, Home, and a character inserted at the front.
    type("cho hi");
    press(KEY.HOME);
    type("e");
    press(KEY.ENTER);
    run(9202);
    s = descriptor(addr);
    if (!rows(s).includes("hi"))
        fail(`sh's editor produced ${JSON.stringify(rows(s))}, expected hi`);

    // A pipeline of two real programs, built by a *process* out of Sys::Pipe
    // and two spawns, and a redirection it opened itself.
    s = submit("clear", 9203);
    s = submit("ls /bin | grep tail", 9204);
    if (!rows(s).includes("tail"))
        fail(`sh's pipeline printed ${JSON.stringify(rows(s))}, expected tail`);

    s = submit("echo written > /home/sh.out", 9205);
    s = submit("cat /home/sh.out", 9206);
    if (!rows(s).includes("written"))
        fail(`sh's redirection produced ${JSON.stringify(rows(s))}`);

    // Its own working directory, moved by its own builtin and inherited by
    // what it spawns — a child of a child of the resident shell.
    s = submit("clear", 9207);
    s = submit("cd /share", 9208);
    cwd = "/share"; // the child's, not the resident shell's
    s = submit("pwd", 9209);
    if (!rows(s).includes("/share"))
        fail(`cd in sh left ${JSON.stringify(rows(s))}, expected /share`);

    // ^C reaches what sh put in front, and sh survives it: the whole point of
    // Sys::Fg. The prompt that comes back is sh's, reporting 130.
    s = submit("clear", 9210);
    type("sleep 60000");
    press(KEY.ENTER);
    run(9211);
    press("c".codePointAt(0), CTRL);
    run(9212);
    s = descriptor(addr);
    if (!rows(s).some((line) => line.startsWith(prompt(130))))
        fail(`^C in sh left ${JSON.stringify(rows(s))}, expected sh's ${prompt(130)}`);

    // Cooked input reaches a child of sh: the pump cooks into the console, and
    // sh gave the console to `cat` by letting go of the keyboard.
    s = submit("clear", 9216.1);
    type("cat");
    press(KEY.ENTER);
    run(9216.2);
    type("typed");
    press(KEY.ENTER);
    run(9216.3);
    press("d".codePointAt(0), CTRL);
    run(9216.4);
    s = descriptor(addr);
    if (rows(s).filter((line) => line === "typed").length !== 2)
        fail(`cat under sh echoed ${JSON.stringify(rows(s))}, expected the line twice`);

    // A background job, its table, and `kill %n` — all sh's own memory now,
    // over Sys::Spawn and Sys::Kill.
    s = submit("clear", 9216.5);
    s = submit("sleep 60000 &", 9216.6);
    if (!rows(s).some((line) => line.startsWith("[1] ")))
        fail(`sh did not announce the job: ${JSON.stringify(rows(s))}`);
    s = submit("jobs", 9216.7);
    if (!rows(s).some((line) => line.startsWith("[1]+ running sleep 60000 &")))
        fail(`sh's jobs printed ${JSON.stringify(rows(s))}`);
    s = submit("kill %1", 9216.8);
    if (!rows(s).some((line) => line.startsWith("[1] interrupt")))
        fail(`kill %n did not report the job: ${JSON.stringify(rows(s))}`);
    s = submit("clear", 9216.9);
    s = submit("jobs", 9216.95);
    if (rows(s).some((line) => line.includes("sleep")))
        fail(`kill %n left the job in the table: ${JSON.stringify(rows(s))}`);

    // A full-screen child claims the keyboard sh let go of, paints, and gives
    // it back — the claim transfer that made Sys::Fg necessary.
    s = submit("clear", 9217);
    type("less /share/doc/README");
    press(KEY.ENTER);
    run(9218);
    s = descriptor(addr);
    if (!rows(s).some((line) => line.includes("/share/doc/README") && line.includes("q quits")))
        fail(`less under sh painted ${JSON.stringify(rows(s))}`);
    press("q".codePointAt(0));
    run(9219);
    s = descriptor(addr);
    if (row(s, s.cursor_y) !== prompt())
        fail(`less did not give sh its screen back: ${JSON.stringify(rows(s))}`);

    // And ^C on one, which is the harder half: the claim is the kernel's, on the
    // killed process's record, so the shell has to get it back from a program
    // that never ran a line of its own cleanup.
    s = submit("clear", 9219.1);
    type("less /share/doc/README");
    press(KEY.ENTER);
    run(9219.2);
    press("c".codePointAt(0), CTRL);
    run(9219.3);
    s = submit("echo alive", 9219.4);
    if (!rows(s).includes("alive"))
        fail(`sh lost the keyboard to a killed full-screen child: ${JSON.stringify(rows(s))}`);

    // And it leaves by its own builtin, back to the resident shell's prompt —
    // which is still where it was, because the `cd` above moved the child's
    // working directory and nobody else's (Concept.md §5.1).
    s = submit("exit", 9213);
    cwd = "/home";
    s = submit("clear", 9214);
    s = submit("pwd", 9215);
    if (rows(s).includes("/share"))
        fail("sh's cd moved the resident shell's working directory");
    s = submit("echo back", 9216);
    if (!rows(s).includes("back"))
        fail(`the resident shell did not come back: ${JSON.stringify(rows(s))}`);

    // exit ends the shell, and nothing runs after it. Last, for that reason.
    s = submit("clear", 9097);
    s = submit("exit 7", 9098);
    if (!rows(s).some((line) => line.startsWith("braam: the shell exited")))
        fail(`exit said nothing: ${JSON.stringify(rows(s))}`);
    s = submit("echo after", 9099);
    if (rows(s).includes("after"))
        fail("a command ran after the shell exited");

    console.log(`smoke ok: ${got_imports.length} imports, ${got_exports.length} exports`);
} else {
    const failures = instance.exports.run_tests();
    if (failures !== 0)
        fail(`${failures} check(s) failed`);
}

// Headless driver for kernel.wasm and tests.wasm. Node stands in for the
// browser: instantiating a freestanding module needs nothing browser-specific,
// and test/fakefs.mjs stands in for OPFS.

import { readFileSync } from "node:fs";
import { basename } from "node:path";

import { E } from "../web/abi.js";
import { FakeStore, makeFakeImports } from "./fakefs.mjs";
import { parseZip } from "../web/fs.js";
import { FakeNet, makeFakeSvc } from "./fakesvc.mjs";
import { pasted } from "../web/keys.js";
import { Renderer } from "../web/render.js";

function usage() {
    console.error("usage: run.mjs --kernel <wasm> [<rootfs.zip> [<proc.wasm>...]] |" +
                  " --tests <wasm>");
    process.exit(2);
}

const [mode, file, rootfs] = process.argv.slice(2);
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

// The archive tools/pack.py just produced, read with the unpacker web/fs.js
// ships, so the packer and the reader are checked against each other rather
// than each against its own idea of the format.
//
// Parsed here rather than inside the import: inflating is asynchronous and the
// fake answers synchronously, so the phase that can await does it once.
if (rootfs)
    store.entries = await parseZip(new Uint8Array(readFileSync(rootfs)));

// One stored (uncompressed) entry, by hand: pack.py always deflates, so this
// is the only exercise method 0 gets, and it is what lets a hostile name be
// put in front of the parser without a packer that would refuse to write one.
function zipOf(name, body) {
    const n = new TextEncoder().encode(name);
    const data = new TextEncoder().encode(body);
    const out = [];
    const put = (...b) => out.push(...b);
    const u16 = (v) => put(v & 0xff, (v >> 8) & 0xff);
    const u32 = (v) => { u16(v & 0xffff); u16((v >>> 16) & 0xffff); };

    u32(0x04034b50); u16(20); u16(0); u16(0); u16(0); u16(0);
    u32(0); u32(data.length); u32(data.length); u16(n.length); u16(0);
    put(...n, ...data);

    const cd = out.length;
    u32(0x02014b50); u16(20); u16(20); u16(0); u16(0); u16(0); u16(0);
    u32(0); u32(data.length); u32(data.length);
    u16(n.length); u16(0); u16(0); u16(0); u16(0); u32(0); u32(0);
    put(...n);

    const end = out.length;
    u32(0x06054b50); u16(0); u16(0); u16(1); u16(1);
    u32(end - cd); u32(cd); u16(0);
    return new Uint8Array(out);
}

const stored = await parseZip(zipOf("share/hello", "hi"));
if (stored.length !== 1 || new TextDecoder().decode(stored[0].bytes) !== "hi")
    throw new Error(`a stored entry did not read back: ${JSON.stringify(stored)}`);

// Concept.md §5.2's store is the whole namespace now, so a name that escapes
// the root is the one zip bug worth checking by hand.
for (const name of ["../escape", "/etc/passwd", "bin/../../out", "C:\\out"]) {
    let refused = false;
    await parseZip(zipOf(name, "x")).catch(() => { refused = true; });
    if (!refused)
        throw new Error(`the unpacker accepted ${JSON.stringify(name)}`);
}

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
// `cells` moves when a scrollback view opens or closes, so a descriptor taken
// before a keystroke must not be read after one.
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

// The names in a listing: ls pads to a shared column width when stdout is the
// grid, so a name is a field of a row rather than a row of its own.
function words(s) {
    return rows(s).flatMap((line) => line.split(/ +/).filter(Boolean));
}

// What one command printed: the rows between the line it was typed on and the
// prompt that followed, blank lines kept — which is what `ls -R` separates its
// blocks with. Meant for a screen that was cleared first.
function output(s) {
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
const SHIFT = 1;

const module = new WebAssembly.Module(readFileSync(file));

function instantiate() {
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

instantiate();

const fail = (msg) => {
    console.error(`${basename(file)}: ${msg}`);
    process.exit(1);
};

const names = (list) => list.map((e) => `${e.module ? e.module + "." : ""}${e.name}`).sort();

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
let ticks = 0;

// The shell is an instance now, and a live one for as long as the system is up
// — a worker of its own with it — so what every assertion below means by "live"
// is "besides the shell". Subtracting it here rather than at sixteen call sites
// keeps them saying what they were written to say. It is exactly one: init
// replaces a shell that died before anything else runs, so the window in which
// there is none is inside a single `run()`.
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
    // Every program is one of these and there is nothing else to be: a worker
    // of its own, the shell included, and no flag in the binary that says
    // otherwise (Concept.md §4).

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
        if (m[0] !== 0x6d617262 || m[1] !== 9)
            fail(`${basename(binary)}'s metadata is ${m[0].toString(16)}/${m[1]}`);
        if (m[4] !== 256)
            fail(`${basename(binary)} asks for ${m[4]} pages, expected 256`);
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

    // First boot: an empty store is unpacked without asking, and what came out
    // of the archive is what the shell was then found in.
    if (rootfs) {
        if (store.unpacks !== 1)
            fail(`the first boot unpacked ${store.unpacks} times, expected 1`);
        if (!store.files.has("/bin/sh"))
            fail("the unpack did not install /bin/sh");
        if (!store.dirs.has("/tmp") || !store.dirs.has("/import"))
            fail("boot did not make the directories the archive does not carry");
        const stamp = new TextDecoder().decode(store.files.get("/version") || new Uint8Array(0));
        if (!/^\d+\.\d+\.\d+/.test(stamp))
            fail(`/version reads ${JSON.stringify(stamp)}, expected a version`);
    }

    // init prints /share/motd before the shell, in green, and the prompt sets
    // its own colour rather than inheriting one. COLOR_GREEN is 2 and
    // COLOR_WHITE|COLOR_BRIGHT is 15, from the enum in src/kernel/screen.h.
    s = descriptor(addr);
    // The geometry is /proc/host's alone. On the banner it would name the very
    // screen it is printed on, and go stale at the first resize.
    if (rows(s).some((line) => line.startsWith("screen:")))
        fail(`the banner reports the geometry: ${JSON.stringify(rows(s))}`);

    const motd_y = rows(s).findIndex((line) => line.startsWith("braam — a small operating system"));
    if (motd_y < 0)
        fail(`the motd did not print at boot: ${JSON.stringify(rows(s))}`);
    if (cell(s, 0, motd_y).fg !== 2)
        fail(`the motd is colour ${cell(s, 0, motd_y).fg}, expected green`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`the boot prompt is ${row(s, s.cursor_y)}, expected ${prompt()}`);
    // The directory is white on blue, the space beside it is not, and the $ is
    // bright white: three of one Sys::Echo's four runs, the fourth being the
    // reset. COLOR_WHITE is 7, COLOR_BLUE is 4 and COLOR_BLACK is 0.
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

    submit("clear", 1045); // the builtins and the programs need a tall grid
    addr = instance.exports.resize(100, 60);
    if (addr === 0)
        fail("the resize before help failed");
    s = submit("help", 1050);
    for (const name of ["break", "cat", "cd", "chat", "clear", "continue", "curl", "date", "df",
                        "echo", "edit",
                        "export", "false", "fg", "grep", "head", "help", "import", "jobs", "kill",
                        "less", "ls", "mkdir", "mount", "pbcopy", "pbpaste", "ps", "pwd",
                        "readonly", "rm", "save", "set", "shift", "sleep",
                        "tail", "timeout", "touch", "true", "uname", "unset", "vmstat",
                        "watch", "wc"])
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
    // And it is red, ahead of the cwd on blue and the bright white $: three of
    // the four runs one Sys::Echo carries. COLOR_RED is 1.
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
    // The editor is a program (Concept.md §4), so every operation it makes is a
    // step of its own; what M2 asked for was one present per *tick*, and that is
    // still exactly what happens. Since Sys::Echo carries the whole repaint,
    // both a keystroke and a whole prompt are one tick.
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

    // §4.4's cost is per operation, so the count is the measurement — and it is
    // measured rather than read off the source, where a wrapper that grew a
    // call would not show. Both spans run from one parked key_read to the next.
    // A keystroke is the Echo that repaints and that key_read. Enter is that
    // Echo, the newline ending the row, the cwd the next prompt names, the Echo
    // that draws it, and the key_read: the directory is asked for every line on
    // purpose, since a stale prompt is believed.
    const calls = () => net.proc.stats().calls;
    let was = calls();
    type("x");
    run(1121);
    if (calls() - was !== 2)
        fail(`a keystroke costs ${calls() - was} round trips, expected 2`);

    press("u".codePointAt(0), CTRL); // an empty line, so the count is the prompt's
    run(1122);
    was = calls();
    press(KEY.ENTER);
    run(1123);
    if (calls() - was !== 5)
        fail(`Enter to the next prompt costs ${calls() - was} round trips, expected 5`);
    s = descriptor(addr);
    if (row(s, s.cursor_y) !== prompt())
        fail(`an empty line left ${row(s, s.cursor_y)}, expected ${prompt()}`);
    type("hi"); // what the resize below reflows
    run(1124);

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
    // archive's binaries, and grep filters the listing, both running at once
    // over a bounded pipe. `clear` first, so the rows below are the
    // pipeline's and nothing else's.
    addr = instance.exports.resize(60, 16);
    if (addr === 0)
        fail("the resize before the pipeline failed");
    press("c".codePointAt(0), CTRL); // the "hi" typed above is still pending
    s = submit("clear", 1130);
    s = submit("ls /bin | grep tai", 1140);
    const listed = rows(s).filter((line) => line && !line.includes("$"));
    if (listed.join() !== "tail")
        fail(`ls /bin | grep tai printed ${JSON.stringify(listed)}, expected ["tail"]`);
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

    // Two spaces: one would survive the word becoming two arguments, since
    // echo joins with a single space.
    s = submit("echo 'a  b'", 1161);
    if (!rows(s).includes("a  b"))
        fail(`echo 'a  b' printed ${JSON.stringify(rows(s))}, expected a  b`);
    s = submit("echo a\\ \\ b", 1162);
    if (!rows(s).includes("a  b"))
        fail(`echo a\\ \\ b printed ${JSON.stringify(rows(s))}, expected a  b`);

    // Variables. The unit suite has the expander; what only a real shell shows
    // is that a value survives to the next line and that the fields reach argv.
    let vt = 1163;
    const vrun = (line) => submit(line, (vt += 0.01));
    const vshows = (line, want) => {
        vrun("clear");
        const got = rows(vrun(line));
        if (!got.includes(want))
            fail(`${line} printed ${JSON.stringify(got)}, expected ${want}`);
    };

    vrun("x=one");
    vshows("echo $x", "one");
    vshows("echo ${x}s", "ones");
    vshows("echo ${nosuch-alt}", "alt");
    vshows("echo ${nosuch?}", "braam: nosuch: parameter not set");

    // Empty against absent, in two spaces: `"$x"` is an argument and `$x` is
    // not, which is the whole of the field flag.
    vrun("e=");
    vshows('echo a "$e" b', "a  b");
    vshows("echo a $e b", "a b");

    // Splitting against IFS, and quoting turning it off.
    vrun('two="a  b"');
    vshows("echo $two", "a b");
    vshows('echo "$two"', "a  b");

    // The positional parameters and $#.
    vrun("set p q r");
    vshows("echo $# $2", "3 q");
    vrun("shift");
    vshows("echo $# $*", "2 q r");

    // $? is the last command's — with no `clear` between, since that would be
    // the last command.
    vrun("clear");
    vrun("false");
    if (!rows(vrun("echo $?")).includes("1"))
        fail("$? did not carry the last command's status");

    // An assignment prefix does not stay: a child has no environment to have
    // seen it either way.
    vrun("x=two echo hi");
    vshows("echo $x", "one");

    // A command name and a redirection target out of a variable: the argv
    // words split and the target does not.
    vrun("c=echo");
    vshows("$c via", "via");
    vrun("f=vfile");
    vrun("echo hi > $f");
    vshows("cat vfile", "hi");
    vrun("rm vfile");

    // readonly bites where export cannot.
    vrun("readonly r=keep");
    vshows("r=other", "braam: r: cannot be set");
    vshows("echo $r", "keep");
    vrun("unset x");
    vshows("echo a $x b", "a b");

    // Lists. The unit suite has the tree; what only a real shell shows is that
    // each pipeline runs in its turn and that a status steers the next.
    vshows("echo one; echo two", "one");
    vshows("echo one; echo two", "two");
    vshows("true && echo yes", "yes");
    vshows("false || echo yes", "yes");
    vshows("false && echo a || echo b", "b");
    vshows("{ echo a; echo b; }", "b");
    vshows("! false && echo yes", "yes");
    vshows("echo kept # dropped", "kept");

    vrun("clear");
    if (rows(vrun("false && echo no")).includes("no"))
        fail("&& ran its right side after a failure");
    vrun("clear");
    if (rows(vrun("true || echo no")).includes("no"))
        fail("|| ran its right side after a success");

    // $? is the pipeline's, not the line's: both of these are one line.
    vshows("false; echo $?", "1");
    vshows("true; echo $?", "0");
    vshows("! true; echo $?", "1");

    // A half-typed construct asks for more, under PS2 rather than the prompt.
    vrun("clear");
    let cont = vrun("echo a &&");
    if (row(cont, cont.cursor_y) !== ">")
        fail(`a trailing && left ${JSON.stringify(row(cont, cont.cursor_y))}, expected >`);
    cont = vrun("echo b");
    if (!rows(cont).includes("a") || !rows(cont).includes("b"))
        fail(`the continuation printed ${JSON.stringify(rows(cont))}, expected a and b`);
    if (row(cont, cont.cursor_y) !== prompt())
        fail(`the continuation did not come back to a prompt`);

    // An unclosed group asks too, and PS2 is a variable.
    vrun("clear");
    vrun("PS2=... ");
    cont = vrun("{ echo in;");
    if (row(cont, cont.cursor_y) !== "...")
        fail(`PS2 left ${JSON.stringify(row(cont, cont.cursor_y))}, expected ...`);
    cont = vrun("}");
    if (!rows(cont).includes("in"))
        fail(`the group printed ${JSON.stringify(rows(cont))}, expected in`);
    vrun("unset PS2");

    // ^C at a continuation throws the accumulation away with the line.
    vrun("clear");
    vrun("echo lost &&");
    press("c".codePointAt(0), CTRL);
    run((vt += 0.01));
    cont = descriptor(addr);
    if (row(cont, cont.cursor_y) !== prompt(130))
        fail(`^C on a continuation left ${JSON.stringify(row(cont, cont.cursor_y))}`);
    cont = vrun("echo fresh");
    if (rows(cont).includes("lost"))
        fail("^C on a continuation kept what had been typed");

    // A background job is listed by its own text, not by the whole line.
    vrun("clear");
    vrun("echo first; sleep 5000 &");
    cont = vrun("jobs");
    if (!rows(cont).some((line) => line.includes("running sleep 5000")))
        fail(`jobs after a list showed ${JSON.stringify(rows(cont))}`);
    if (rows(cont).some((line) => line.includes("running echo first")))
        fail("a list's first pipeline was filed as a job");
    vrun("kill %1");
    vrun("clear");

    // Control flow. The unit suite has the tree; what only a real shell shows
    // is that a loop rebinds its variable and that break and continue reach
    // the loop from inside a pipeline.
    vshows("for i in a b c; do echo $i; done", "a");
    vshows("for i in a b c; do echo $i; done", "c");
    vrun("clear");
    if (rows(vrun("for i in; do echo x; done")).includes("x"))
        fail("a for over an empty list ran its body");
    vrun("set p q");
    vshows("for i; do echo $i; done", "q"); // no `in`: the positional parameters
    vrun("set --");

    vshows("if true; then echo yes; else echo no; fi", "yes");
    vshows("if false; then echo no; else echo yes; fi", "yes");
    vshows("if false; then echo no; elif true; then echo yes; fi", "yes");
    vrun("clear");
    if (rows(vrun("if false; then echo no; fi")).includes("no"))
        fail("an if ran a branch its condition refused");

    // POSIX rather than v7: an if that takes no branch reports 0, where v7
    // leaves the failed condition's status behind.
    vshows("if false; then echo x; fi; echo $?", "0");
    vshows("for i in a; do false; done; echo $?", "1");
    vshows("while false; do echo x; done; echo $?", "0");

    vshows("while true; do echo once; break; done", "once");
    vshows("until false; do echo once; break; done", "once");
    vrun("clear");
    if (rows(vrun("for i in a b c; do continue; echo skipped; done")).includes("skipped"))
        fail("continue did not start the next turn");

    // `break 2` leaves both, and leaves nothing behind for the next line.
    // Spelled without spaces because the driver types a line in one burst and
    // the key ring holds 64: a human types slowly enough not to care.
    vrun("clear");
    let looped = rows(vrun("for i in a b;do for j in c d;do echo $i$j;break 2;done;done"));
    if (!looped.includes("ac"))
        fail(`a nested loop printed ${JSON.stringify(looped)}, expected ac`);
    if (looped.includes("ad") || looped.includes("bc"))
        fail(`break 2 left a loop running: ${JSON.stringify(looped)}`);
    vshows("echo after", "after");

    // Outside a loop both are silent no-ops, as they are in v7.
    vshows("break; echo still", "still");
    vshows("continue 3; echo still", "still");

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

    // One file named twice. §5.2 used to refuse the second open outright; Input
    // now opens the second only after closing the first.
    s = submit("clear", 1173.1);
    s = submit("cat notes notes", 1173.2);
    const twice = rows(s).filter((line) => line && !line.includes("$"));
    if (twice.join(",") !== "one,two,one,two")
        fail(`cat notes notes printed ${JSON.stringify(twice)}, expected one,two,one,two`);

    // Two descriptors on one file at the same moment, which laziness alone does
    // not fix: the shell opens notes for the stage's stdin and Sys::Spawn moves
    // that handle into grep, which then opens notes again for itself.
    s = submit("clear", 1173.3);
    s = submit("grep one notes < notes", 1173.4);
    if (!rows(s).includes("one"))
        fail(`grep with a redirection on its own file printed ${JSON.stringify(rows(s))}`);

    // A redirection that cannot be opened stops the command before it runs.
    // /proc is the read-only mount now that the store holds everything else.
    s = submit("echo hi > /proc/uptime", 1174);
    if (!rows(s).some((line) => line.startsWith("braam: /proc/uptime: ")))
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

    // /bin and /share are directories in the store, unpacked at boot from the
    // archive tools/pack.py wrote at the end of the build.
    if (rootfs) {
        s = submit("clear", 1183);
        s = submit("cat /share/motd", 1184);
        if (!rows(s).some((line) => line.startsWith("braam —")))
            fail(`/share/motd did not read back: ${JSON.stringify(rows(s))}`);
        s = submit("clear", 1185);
        // Two names, and the grid is stdout, so they share a row.
        s = submit("ls /share", 1186);
        if (output(s).join("|") !== "help  motd")
            fail(`/share did not list its files: ${JSON.stringify(output(s))}`);
        // The README is at the root, where somebody arriving will see it. Not
        // the whole row: what else is at the top level moves with boot.cpp.
        s = submit("clear", 1186.1);
        s = submit("ls /", 1186.2);
        if (!words(s).includes("README"))
            fail(`/ did not list the README: ${JSON.stringify(output(s))}`);
    }

    // The layout itself, on a fixture the suite owns: /bin re-breaks whenever a
    // program is added. Sizes are chosen so name order, size order and reversed
    // order all differ. After vmstat, which counts the syscalls these make.
    submit("mkdir /home/t", 1186.30);
    submit("mkdir /home/t/sub", 1186.31);
    submit("echo 1 > /home/t/aaa", 1186.32); // 2 bytes
    submit("echo 123456 > /home/t/bb", 1186.33); // 7 bytes

    // aaa=3, bb=2, sub/=4, so the column is 4 + 2 wide and three of them fit.
    const listing = (line, now) => {
        submit("clear", now);
        return output(submit(line, now + 0.005)).join("|");
    };
    const listings = [
        ["ls /home/t", "aaa   bb    sub/"],
        ["ls -1 /home/t", "aaa|bb|sub/"],
        ["ls -l /home/t", "total 2|file 2 aaa|file 7 bb|dir  0 sub/"],
        ["ls -lh /home/t", "total 2|file 2B aaa|file 7B bb|dir  0B sub/"],
        ["ls -S /home/t", "bb    aaa   sub/"],
        ["ls -r /home/t", "sub/  bb    aaa"],
        ["ls -rS /home/t", "sub/  aaa   bb"],
        ["ls -d /home/t", "/home/t/"],
        // Bundled, and a named directory gets no `total`.
        ["ls -dl /home/t", "dir  0 /home/t/"],
        ["ls -- /home/t", "aaa   bb    sub/"],
        ["ls -R /home/t", "/home/t:|aaa   bb    sub/||/home/t/sub:"],
        ["ls -lR /home/t",
            "/home/t:|total 2|file 2 aaa|file 7 bb|dir  0 sub/||/home/t/sub:|total 0"],
        // A file operand prints before a directory one, in a block of its own.
        ["ls /home/t/aaa /home/t", "/home/t/aaa||/home/t:|aaa   bb    sub/"],
        // A pipe is one name per line; -C forces columns into one, at eighty.
        ["ls /home/t | head -n 2", "aaa|bb"],
        ["ls -C /home/t | cat", "aaa   bb    sub/"],
    ];
    let at = 1186.4;
    for (const [line, want] of listings) {
        const got = listing(line, at);
        at += 0.02;
        if (got !== want)
            fail(`\`${line}\` printed ${JSON.stringify(got)}, expected ${JSON.stringify(want)}`);
    }

    // A name is as wide as its codepoints, not its bytes: the grid puts one
    // codepoint in one cell, so padding by bytes shifts every column after an
    // accented name left by one. abcdef is six cells and six bytes, naïve five
    // cells and six, which is what tells the two apart.
    submit("mkdir /home/u", 1186.75);
    submit("touch /home/u/abcdef /home/u/naïve /home/u/xy", 1186.76);
    submit("clear", 1186.77);
    const wide = output(submit("ls /home/u", 1186.78)).join("|");
    if (wide !== "abcdef  naïve   xy")
        fail(`a UTF-8 name misaligned the columns: ${JSON.stringify(wide)}`);
    submit("rm -r /home/u", 1186.79);

    s = submit("clear", 1186.8);
    s = submit("ls -z /home/t", 1186.81);
    if (!output(s).some((line) => line.startsWith("ls: illegal option -- z")))
        fail(`an unknown flag said nothing: ${JSON.stringify(output(s))}`);
    if (!rows(s).includes(prompt(2)))
        fail(`an unknown flag left ${row(s, s.cursor_y)}, expected ${prompt(2)}`);

    submit("rm -r /home/t", 1186.9);

    // M5, second criterion, as amended: df reports the quota and the usage as
    // a BSD table, the durability having moved to the boot banner. The whole
    // line is matched, since a row wider than this grid's sixty columns wraps.
    const blocks = Math.floor(store.quota / 1024);
    s = submit("clear", 1180);
    s = submit("df", 1181);
    const df = rows(s);
    if (!df.includes("Filesystem  1K-blocks     Used    Avail Capacity  Mounted on"))
        fail(`df did not head the table: ${JSON.stringify(df)}`);
    const root = df.find((line) => line.endsWith("    /"));
    if (!root || !new RegExp(`^opfs +${blocks} +\\d+ +\\d+ +\\d+%    /$`).test(root))
        fail(`df did not report the root: ${JSON.stringify(df)}`);
    if (!df.some((line) => /^procfs +0 +0 +0 +-    \/proc$/.test(line)))
        fail(`df did not report /proc: ${JSON.stringify(df)}`);

    // -h, over the fake's ten gibibytes exactly — the same figure as above.
    s = submit("clear", 1181.1);
    s = submit("df -h", 1181.2);
    const dfh = rows(s);
    if (!dfh.includes("Filesystem       Size     Used    Avail Capacity  Mounted on"))
        fail(`df -h did not head the table: ${JSON.stringify(dfh)}`);
    if (!dfh.some((line) => /^opfs +10G +\d/.test(line)))
        fail(`df -h did not scale the quota: ${JSON.stringify(dfh)}`);

    // /proc/host is what the kernel knows about itself and what the host said
    // about the browser at boot, and `uname` reformats it — the arrangement
    // `mount` has over /proc/mounts. The host half comes from the fake's fixed
    // string, so the whole file is deterministic.
    s = submit("clear", 1182);
    s = submit("cat /proc/host", 1183);
    const host = rows(s);
    // A plain table, colons and all left to the boot banner: `uname` reads a
    // field out of this with next_field, so a name must not carry punctuation.
    if (host.some((line) => /^[a-z]+:/.test(line)))
        fail(`/proc/host punctuates its names: ${JSON.stringify(host)}`);
    for (const want of ["system   braam", "machine  wasm32", "browser  Fake 1", "agent    fake"])
        if (!host.includes(want))
            fail(`/proc/host is missing ${JSON.stringify(want)}: ${JSON.stringify(host)}`);
    if (!host.some((line) => /^release  \d+\.\d+\.\d+/.test(line)))
        fail(`/proc/host did not report the release: ${JSON.stringify(host)}`);
    if (!host.includes(`screen   ${s.cols}x${s.rows}`))
        fail(`/proc/host did not report the geometry: ${JSON.stringify(host)}`);

    // The default is the system name; -m is the one field neither the host nor
    // the version supplies.
    s = submit("clear", 1184.1);
    s = submit("uname", 1184.2);
    if (!rows(s).includes("braam"))
        fail(`uname printed ${JSON.stringify(rows(s))}, expected braam`);
    s = submit("uname -m", 1184.3);
    if (!rows(s).includes("wasm32"))
        fail(`uname -m printed ${JSON.stringify(rows(s))}, expected wasm32`);

    // -a is every field, and the blank line splitting the banner's half from
    // the rest is a marker rather than something to print.
    s = submit("clear", 1184.4);
    s = submit("uname -a", 1184.5);
    const all = rows(s).filter((line) => line && !line.includes("$"));
    if (!all.includes("system   braam") || !all.includes("agent    fake"))
        fail(`uname -a printed ${JSON.stringify(all)}`);
    s = submit("uname -z", 1184.6);
    if (!rows(s).some((line) => line.startsWith("usage: uname ")))
        fail(`uname -z printed ${JSON.stringify(rows(s))}, expected a usage line`);
    if (!rows(s).includes(prompt(2)))
        fail(`uname -z left ${row(s, s.cursor_y)}, expected ${prompt(2)}`);

    // /proc/tasks is every task in one open, so no two rows describe different
    // moments, and `ps` reformats it the way `df` reformats /proc/mounts. Both
    // are wider than this grid, so it goes wide the way it does for help.
    submit("clear", 1185);
    addr = instance.exports.resize(100, 48);
    if (addr === 0)
        fail("the resize before ps failed");

    // The pump is task 1 and holds no worker: a task without one is a coroutine
    // in the kernel, which is the whole of what the browser could never say.
    s = submit("cat /proc/tasks", 1185.1);
    const tasks = rows(s).filter((line) => line && !line.includes("$"));
    if (!tasks.some((line) => /^1 tty \w+ \S+ \S+ - 0 0 0 0 0 \d+ -$/.test(line)))
        fail(`/proc/tasks has no pump with no worker: ${JSON.stringify(tasks)}`);
    if (!tasks.some((line) => /^2 init \w+ \S+ \S+ bound 0 \d+ \d+ \d+ \d+ \d+ \/home$/.test(line)))
        fail(`/proc/tasks has no init holding a worker: ${JSON.stringify(tasks)}`);
    for (const line of tasks)
        if (line.split(" ").length !== 13)
            fail(`/proc/tasks is not thirteen fields: ${JSON.stringify(line)}`);

    // The memory a process has committed is measured on the host and rides back
    // on every step: less than the cap, and not nought, since a running instance
    // has pages. Nothing else in a browser will say what a worker holds.
    const shell = tasks.find((line) => line.startsWith("2 init "));
    const [used, cap] = shell.split(" ").slice(9, 11).map(Number);
    if (!(used > 0 && used < cap))
        fail(`init committed ${used} of ${cap} bytes, expected some of it`);
    if (cap !== 256 * 65536)
        fail(`init's cap is ${cap}, expected PROC_MAX_PAGES`);

    // ps itself is one of the tasks it lists — it is a process like any other,
    // and the shell armed it as the foreground before waiting for it. A syscall
    // server is a task too, and names the process it serves rather than floating.
    s = submit("ps", 1185.2);
    const ps = rows(s);
    if (!ps.includes("  PID PPID NAME         STAT WORKER WAIT   CALLS FDS   MEM ELAPSED  CWD"))
        fail(`ps did not head the table: ${JSON.stringify(ps)}`);
    if (!ps.some((line) => /^ +1 +- tty +[RS] +- +\S+ +- +- +- +\d+:\d\d +-$/.test(line)))
        fail(`ps did not show the pump as workerless: ${JSON.stringify(ps)}`);
    if (!ps.some((line) =>
        /^ +2 +- init +[RS] +bound +\S+ +\d+ +\d+ +\d+(\.\d)?[BKM] +\d+:\d\d +\/home$/.test(line)))
        fail(`ps did not scale init's memory: ${JSON.stringify(ps)}`);
    if (!ps.some((line) => /^ +\d+ +2 ps +S\+ +bound /.test(line)))
        fail(`ps did not list itself in the foreground: ${JSON.stringify(ps)}`);
    if (!ps.some((line) => /^ +\d+ +\d+ \/bin\/ps +[RS] +- /.test(line)))
        fail(`ps did not attribute its own syscall server: ${JSON.stringify(ps)}`);
    s = submit("ps -x", 1185.3);
    if (!rows(s).includes("usage: ps") || !rows(s).includes(prompt(2)))
        fail(`ps -x printed ${JSON.stringify(rows(s))}, expected a usage line`);

    // vmstat is the same counters as rates. This is the only place the rate
    // arithmetic runs at all: the in-wasm suite cannot step a program.
    s = submit("clear", 1185.4);
    s = submit("vmstat", 1185.5);
    const vm = rows(s).filter((line) => line && !line.includes("$"));
    console.error("VMSTAT-DEFAULT:\n" + vm.join("\n"));
    if (!vm[0] || !/^-+procs-+\s+-+memory-+\s+-+alloc-+\s+-+faults-+\s+-*loop-*$/.test(vm[0]))
        fail(`vmstat did not rule its groups: ${JSON.stringify(vm)}`);
    if (vm[1] !== "  r  t  h  p      use    fre      al    fr  gr      in    sy    cs      tk")
        fail(`vmstat did not name its columns: ${JSON.stringify(vm[1])}`);
    // Thirteen numbers. vmstat is itself one of the tasks it counts — the syscall
    // server that generated the file is runnable and the stepper is parked on its
    // reply — so `r` and `p` cannot be nought while it is the one asking.
    const cells = (vm[2] || "").trim().split(/ +/).map(Number);
    if (cells.length !== 13 || cells.some((n) => !Number.isFinite(n)))
        fail(`vmstat's row is not thirteen numbers: ${JSON.stringify(vm[2])}`);
    if (!(cells[0] >= 1 && cells[3] >= 1))
        fail(`vmstat counted itself out of its own row: ${JSON.stringify(vm[2])}`);
    if (!(cells[4] > 0 && cells[5] > 0))
        fail(`vmstat reported no heap: ${JSON.stringify(vm[2])}`);

    // -s is the same file, one counter per line with what it means. Its totals
    // are cumulative, so a syscall count since boot cannot be nought.
    s = submit("clear", 1185.6);
    s = submit("vmstat -s | grep syscalls", 1185.7);
    const sum = rows(s).filter((line) => line.includes("syscalls"));
    console.error("VMSTAT-SUM:\n" + rows(s).join("\n"));
    if (!sum.some((line) => /^ *[1-9]\d* syscalls parked and answered$/.test(line)))
        fail(`vmstat -s did not total the syscalls: ${JSON.stringify(rows(s))}`);

    s = submit("vmstat -s 1", 1185.8);
    if (!rows(s).includes("usage: vmstat [-s] [-w <secs>] [-c <count>] [<secs> [<count>]]"))
        fail(`vmstat -s with an interval printed ${JSON.stringify(rows(s))}`);

    // BSD's other spelling of the same thing, and -c counts the first row, so
    // one repetition never sleeps.
    s = submit("clear", 1185.9);
    s = submit("vmstat -w 1 -c 1", 1186.1);
    const once = rows(s).filter((line) => line && !line.includes("$"));
    if (once.length !== 3)
        fail(`vmstat -w 1 -c 1 printed ${once.length} lines, expected a header and a row`);

    // Two rows an interval apart. The interval is a second, so the second row
    // needs a tick past the deadline the sleep put on the timer queue — which is
    // why this is not one submit.
    s = submit("clear", 1186.2);
    type("vmstat 1 2");
    press(KEY.ENTER);
    run(1186.3);
    run(2186.4); // past the second the first row asked to wait
    s = descriptor(addr);
    const paced = rows(s).filter((line) => line && !line.includes("$"));
    console.error("VMSTAT-PACED:\n" + paced.join("\n"));
    if (paced.length !== 4)
        fail(`vmstat 1 2 printed ${JSON.stringify(paced)}, expected a header and two rows`);
    // The interval came from the file's own clock, so the second row's rates are
    // over ten milliseconds rather than over the whole uptime.
    if (paced[2] === paced[3])
        fail(`vmstat's second row repeated the first: ${JSON.stringify(paced)}`);

    addr = instance.exports.resize(60, 16);
    if (addr === 0)
        fail("the resize after ps failed");

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
    if (!words(s).includes("notes"))
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

    // The stamp still matches, so the archive is not fetched, let alone
    // written: the steady state costs no download at all.
    if (rootfs && store.unpacks !== 1)
        fail(`a reload on a current store unpacked again (${store.unpacks} in all)`);

    // /tmp is the exception to the store's persistence, and emptying it at
    // boot is the whole of what makes it temporary now.
    submit("echo scratch > /tmp/note", 2020);
    if (!store.files.has("/tmp/note"))
        fail("/tmp is not writable");
    store.reopen();
    instantiate();
    instance.exports.init(0);
    addr = instance.exports.resize(60, 16);
    run(2100);
    if (store.files.has("/tmp/note"))
        fail("boot did not wipe /tmp");
    if (!store.files.has("/home/notes"))
        fail("wiping /tmp took /home with it");

    // A stored image from another kernel is the user's to keep or replace, and
    // declining leaves what is there — including the binaries the shell that
    // is about to run comes out of.
    if (rootfs) {
        store.files.set("/version", new TextEncoder().encode("0.0.1-stale"));
        store.files.set("/bin/keepme", new Uint8Array(1));
        store.reopen();
        instantiate();
        instance.exports.init(0);
        addr = instance.exports.resize(60, 16);
        run(2200);
        s = descriptor(addr);
        if (!rows(s).some((line) => line.includes("0.0.1-stale")))
            fail(`a stale stamp went unmentioned: ${JSON.stringify(rows(s))}`);
        if (!rows(s).some((line) => line.includes("replace /bin and /share?")))
            fail(`boot did not ask before overwriting: ${JSON.stringify(rows(s))}`);

        press("n".codePointAt(0));
        run(2210);
        if (store.unpacks !== 1)
            fail("a declined upgrade unpacked anyway");
        if (!store.files.has("/bin/keepme"))
            fail("a declined upgrade replaced /bin regardless");
        s = descriptor(addr);
        if (row(s, s.cursor_y) !== prompt())
            fail(`declining left ${row(s, s.cursor_y)}, expected a prompt`);

        // And accepting replaces both directories: what the archive does not
        // carry goes, and what it never names is untouched.
        store.reopen();
        instantiate();
        instance.exports.init(0);
        addr = instance.exports.resize(60, 16);
        run(2300);
        press("y".codePointAt(0));
        run(2310);
        if (store.unpacks !== 2)
            fail(`an accepted upgrade unpacked ${store.unpacks - 1} times, expected 2 in all`);
        if (store.files.has("/bin/keepme"))
            fail("the unpack left a binary the archive does not carry");
        if (!store.files.has("/home/notes"))
            fail("the unpack reached outside the directories the archive names");
        s = descriptor(addr);
        if (row(s, s.cursor_y) !== prompt())
            fail(`accepting left ${row(s, s.cursor_y)}, expected a prompt`);
    }

    // M5's third criterion, retired: there is no memory fallback any more, so
    // a browser with no OPFS is told it cannot run rather than given a store
    // that quietly loses everything at the next reload.
    const entries = store.entries;
    store.reset();
    store.entries = entries; // served beside kernel.wasm; a reload still finds it
    store.opfs = false;
    store.sync = false;
    instantiate();
    instance.exports.init(0);
    addr = instance.exports.resize(60, 16);
    run(3000);
    s = descriptor(addr);
    if (!rows(s).some((line) => line.startsWith("braam: this browser has no OPFS")))
        fail(`booting without OPFS said nothing: ${JSON.stringify(rows(s))}`);
    if (store.unpacks !== 0)
        fail("a system with no store unpacked into it anyway");
    if (rows(s).some((line) => line.includes("$")))
        fail(`booting without OPFS reached a prompt: ${JSON.stringify(rows(s))}`);

    // Back to a working store for what follows: the reset above emptied it, so
    // this boot is a first boot again and unpacks without asking.
    store.opfs = true;
    store.sync = true;
    instantiate();
    instance.exports.init(0);
    addr = instance.exports.resize(60, 16);
    run(3005);
    s = descriptor(addr);
    if (row(s, s.cursor_y) !== prompt())
        fail(`the store came back but the shell did not: ${JSON.stringify(rows(s))}`);

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

    // M6, third criterion: /import takes what the picker hands over, and
    // save sends a file back out through the browser.
    net.reset();
    s = submit("clear", 3040);
    s = submit("import", 3041);
    if (!rows(s).includes("/import/notes.txt"))
        fail(`import named nothing: ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3042);
    s = submit("cat /import/notes.txt", 3043);
    if (!rows(s).includes("picked"))
        fail(`the imported file did not read back: ${JSON.stringify(rows(s))}`);

    submit("save /import/notes.txt", 3044);
    if (net.saved.length !== 1 || net.saved[0].name !== "notes.txt")
        fail(`save saved ${JSON.stringify(net.saved.map((f) => f.name))}`);
    if (new TextDecoder().decode(net.saved[0].bytes) !== "picked\n")
        fail(`save saved the wrong bytes: ${JSON.stringify(net.saved[0].bytes)}`);

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
    // The archive's README is the input because it is longer than the pane, which
    // is what makes PgDn mean anything.
    s = submit("clear", 3077);
    s = submit("cat /README | less", 3078);
    // Named once: the two checks below are the same row before and after a
    // scroll, and pinning the text twice let one of them go stale.
    const readme_top = "Braam is a small operating system in a browser tab.";
    if (!rows(s).some((line) => line.startsWith(" stdin ")))
        fail(`less drew no status line: ${JSON.stringify(rows(s))}`);
    if (row(s, 0) !== readme_top)
        fail(`less painted ${JSON.stringify(rows(s))}`);
    press(KEY.PAGE_DOWN);
    run(3079);
    s = descriptor(addr);
    if (row(s, 0) === readme_top)
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
    s = submit("less /README | less", 3062);
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
    if (!rows(s).some((line) => line.startsWith("[1]+ running sleep 5000")))
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

    // M8. Everything above this line already ran a program in an instance of
    // its own without saying so: `wc` is a binary in /bin now, and
    // `echo 'a b' | wc`, `wc < notes` and `curl /hello.txt | wc` are the
    // assertions M4, M5 and M6 wrote against the applet, unchanged. That is the
    // third criterion.

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
        fail(`a two-stage pipeline printed ${JSON.stringify(rows(s))}`);
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

    // And ^C on a pipeline, which is the harder half of Sys::Fg: the shell arms
    // its stages one at a time, having let the keyboard go before it spawned —
    // so by the second call something is in front and the caller owns neither
    // the keys nor what is there. Both stages have to be cancelled, or the
    // prompt comes back with one still reading.
    type("cat | wc");
    press(KEY.ENTER);
    if (run(9023) !== -1)
        fail("a two-stage pipeline did not park");
    if (others() !== 2)
        fail(`${others()} instances for a two-stage pipeline, expected 2`);
    s = submit("clear", 9024); // the ^C below needs the stages still running
    press("c".codePointAt(0), CTRL);
    if (run(9025) !== -1)
        fail("^C on a pipeline left the scheduler with work to do");
    s = descriptor(addr);
    if (!rows(s).includes(prompt(130)))
        fail(`^C on a pipeline left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);
    if (others() !== 0)
        fail(`${others()} stages outlived a ^C on the pipeline`);

    // A file that is not a program is refused before anything runs, and says
    // so differently from a name that is not there at all.
    s = submit("clear", 9030);
    s = submit("/share/motd", 9031);
    if (!rows(s).some((line) => line.startsWith("braam: /share/motd: not executable")))
        fail(`a non-binary was not refused: ${JSON.stringify(rows(s))}`);
    if (!rows(s).includes(prompt(126)))
        fail(`a non-binary left ${row(s, s.cursor_y)}, expected ${prompt(126)}`);

    // help lists what is runnable.
    addr = instance.exports.resize(100, 48);
    s = submit("clear", 9040);
    s = submit("help", 9041);
    for (const name of ["echo", "hog", "sleep", "spin", "tail", "wc"])
        if (!rows(s).some((line) => line.startsWith(`  ${name} `)))
            fail(`help did not list ${name}`);
    addr = instance.exports.resize(60, 16);

    // M9. Every program runs in a worker of its own, the shell included, so
    // everything above this line already did — those assertions are M4's to
    // M8's, unedited, and this is the only line that notices. A latch on
    // "anything ran": the shell binds a worker at boot, before a command is
    // typed.
    if (!net.bound.length)
        fail("nothing bound a worker");

    // One end to end: getpid answered inside its own worker, a write relayed
    // back through the kernel, an exit status carried on the step that
    // reported it.
    net.terminated.length = 0;
    s = submit("clear", 9050);
    s = submit("spin 1", 9051);
    if (!rows(s).some((line) => /^spin: pid \d+, spinning briefly$/.test(line)))
        fail(`a program printed ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`spin 1 exited ${row(s, s.cursor_y)}, expected a bare prompt`);
    if (net.terminated.length !== 0)
        fail("a process that exited had its worker terminated rather than pooled");
    // Every hire this run has made, less the workers that went with the
    // processes it killed, less the one the shell is holding: 18, 16 and 1
    // here. The pool grows only for a pipeline wider than what is idle and
    // shrinks only on a kill, so spin's own is in it — put back rather than
    // terminated, which is the assertion above. The shell's is not: it is a
    // process for as long as the system is up, so one worker is out of the pool
    // from boot and each of the reboots above terminated the rest.
    if (net.proc.pooled() !== 1)
        fail(`the pool holds ${net.proc.pooled()} workers, expected one`);

    // M9, first criterion: a program that does not come back. The fake link
    // leaves its step undelivered, which is all the kernel ever sees of a real
    // loop — there is no reply, no timer, and nothing to cancel but the proxy.
    //
    // `net.hold(n)` counts binds from here (test/fakeworker.mjs), so what falls
    // between a hold and the command it was aimed at is what has to be counted.
    // The shell is not one of them: it binds at boot and at a respawn, never per
    // command. `clear` and a spawning program's own worker are, which is why
    // some of the holds below clear the screen first and some ask for the second.
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
    // Backgrounded, since a foreground job is waited for — what is being
    // asserted is that the kernel is free, not that the shell is rude.
    // The clear comes first: it is a program too, so a hold taken before it
    // would land on its worker rather than on the one that spins.
    s = submit("clear", 9070);
    net.hold();
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

    // A child gets the worker kill through its parent, which is the proof that
    // a spawned process is an ordinary scheduler job.
    //
    // The pool has to be warm before the link is held: `spin` with no argument
    // never comes back, and a spawn that cannot be given a worker backs off and
    // asks again (§4) rather than running the loop anywhere the driver would
    // have to step it.
    s = submit("spin 1", 9155);
    if (net.proc.pooled() !== 2)
        fail(`the pool holds ${net.proc.pooled()} workers, expected two — 23 hired by`
             + " here, 20 terminated with the processes above that were killed, and one"
             + " held by the shell for as long as the system is up");

    net.terminated.length = 0;
    s = submit("clear", 9160);
    net.hold(2); // the parent binds a worker first, and it is the child that loops
    type("timeout 20 spin");
    press(KEY.ENTER);
    run(9161);
    if (others() !== 2)
        fail(`a supervised child left ${others()} instances, expected 2`);
    run(9200); // past the delay, so the kill is the alarm's and not ^C's
    net.release();
    run(9201);
    if (net.terminated.length !== 1)
        fail(`timeout over a child terminated ${net.terminated.length} workers, expected 1`);
    if (others() !== 0)
        fail(`${others()} instances outlived a killed child`);

    // ^C reaches a whole chain: the shell cancels the stage, the stage's End
    // cancels the child, and neither is left behind.
    s = submit("spin 1", 9205); // warm the pool again, for the reason above
    s = submit("clear", 9210);
    net.hold(2);
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

    // The builtins are the shell's own state, so they are not files: `cd` is
    // not in /bin and never resolves through it, and `help` prints them ahead
    // of everything else, with the usage line the table carries.
    addr = instance.exports.resize(100, 60);
    s = submit("clear", 9090);
    s = submit("help", 9091);
    const helped = rows(s).filter((line) => line.startsWith("  "));
    for (const [i, name] of ["break", "cd", "continue", "exit", "export", "fg", "help", "jobs",
                             "kill", "readonly", "set", "shift", "unset"].entries())
        if (!helped[i] || !helped[i].startsWith(`  ${name} `))
            fail(`help listed ${JSON.stringify(helped[i])} at ${i}, expected ${name}`);
    if (!helped.some((line) => line.startsWith("  wc ") && line.includes("count lines")))
        fail(`help did not carry /share/help's usage for wc: ${JSON.stringify(helped)}`);
    addr = instance.exports.resize(60, 16);

    s = submit("clear", 9092);
    s = submit("ls /bin", 9093);
    if (words(s).includes("cd"))
        fail("cd is a builtin and must not be a file in /bin");
    if (!words(s).includes("timeout"))
        fail(`ls /bin lost a binary: ${JSON.stringify(output(s))}`);

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
    if (!rows(s).some((line) => line.startsWith("[1]+ running sleep 60000")))
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
    type("less /README");
    press(KEY.ENTER);
    run(9218);
    s = descriptor(addr);
    if (!rows(s).some((line) => line.includes("/README") && line.includes("q quits")))
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
    type("less /README");
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

    // A prompt line long enough to wrap off the bottom of the screen, which is
    // the one path `Sys::Echo`'s `scrolled` replaced: the anchor's row goes up
    // *under* the write, and the editor has to follow it or every keystroke
    // after it repaints a row that has moved. A narrow, short grid rather than
    // a very long line, so the wrap is three rows and not thirty.
    // The count rather than the layout: a repaint that painted from an anchor
    // the scroll had moved would leave a stale row behind or blank a live one,
    // and either shows up here. Fewer keys than the 64-slot ring holds, since
    // `type` posts them all at once and `key()` refuses a full one.
    addr = instance.exports.resize(20, 5);
    s = submit("clear", 9230);
    s = submit("echo a", 9230.1); // the anchor two rows down, so it survives
    const xs = (t) => (rows(t).join("").match(/x/g) || []).length;
    for (let i = 0; i < 56; i++)
        press("x".codePointAt(0));
    run(9230.2);
    s = descriptor(addr);
    if (xs(s) !== 56)
        fail(`a line wrapped past the bottom shows ${xs(s)} of 56: `
             + JSON.stringify(rows(s)));
    if (s.cursor_y !== 4)
        fail(`the wrapped line left the cursor on row ${s.cursor_y}, expected the last`);

    // And a repaint of the same line after it has scrolled: backspace walks the
    // cursor back over two row boundaries and blanks the tail by hand, which it
    // can only do against an anchor that went up with the grid.
    for (let i = 0; i < 30; i++)
        press(KEY.BACKSPACE);
    run(9230.3);
    s = descriptor(addr);
    if (xs(s) !== 26)
        fail(`backspacing a wrapped line left ${xs(s)} of 26: ${JSON.stringify(rows(s))}`);

    press("c".codePointAt(0), CTRL);
    run(9230.35);
    addr = instance.exports.resize(60, 16);
    s = submit("clear", 9230.4);
    s = submit("echo narrow", 9230.5);
    if (!rows(s).includes("narrow"))
        fail(`the shell did not survive the narrow screen: ${JSON.stringify(rows(s))}`);

    // SYS_ECHO_FRESH: output that does not end its row leaves the cursor mid-
    // line, and the prompt must open one of its own rather than land beside it.
    s = submit("echo -n tail", 9230.6);
    if (!rows(s).some((line) => line === "tail"))
        fail(`echo -n did not get a row of its own: ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`the prompt after echo -n is ${row(s, s.cursor_y)}, expected ${prompt()}`);

    // And the newline that opens that row goes out before any run's style, so a
    // scroll blanks the new bottom row in the default colour. It did not when
    // the newline rode with the first coloured run: screen.cpp's blank() takes
    // the sticky colour, so every prompt at the bottom of a full screen left a
    // blue bar from the $ to the right margin.
    addr = instance.exports.resize(24, 4);
    s = submit("clear", 9230.7);
    for (let i = 0; i < 6; i++)
        s = submit("echo -n filling", 9230.71 + i / 100);
    for (let x = prompt().length; x < s.cols; x++)
        if (cell(s, x, s.cursor_y).bg !== 0)
            fail(`column ${x} past the prompt is on ${cell(s, x, s.cursor_y).bg}, expected black`);
    addr = instance.exports.resize(60, 16);
    s = submit("clear", 9230.8);

    // ^L: the screen goes, the prompt is redrawn at the top with the line still
    // on it, and the cursor lands after what was typed. That last part used to
    // be a cursor_set of its own after the prompt; SYS_ECHO_END is what places
    // it now, and this is the case that says so.
    s = submit("echo scrollback", 9230.9);
    type("keep me");
    run(9230.91);
    press("l".codePointAt(0), CTRL);
    run(9230.92);
    s = descriptor(addr);
    if (rows(s).some((line) => line.includes("scrollback")))
        fail(`^L left the screen behind: ${JSON.stringify(rows(s))}`);
    if (s.cursor_y !== 0)
        fail(`^L drew the prompt on row ${s.cursor_y}, expected the top`);
    if (row(s, 0) !== `${prompt()} keep me`)
        fail(`^L left ${JSON.stringify(row(s, 0))}, expected the line`);
    if (s.cursor_x !== prompt().length + 1 + "keep me".length)
        fail(`^L left the cursor at column ${s.cursor_x}`);
    press("c".codePointAt(0), CTRL);
    run(9230.93);

    // Scrollback. Sixteen echoes on a sixteen-row screen put seventeen rows off
    // the top, so two presses of half a screen land the first of them on the
    // top row. `s.cells` is a composed block while a view is up, which is why
    // the descriptor is re-read after every one of these.
    s = submit("clear", 9230.94);
    for (let i = 0; i < 16; i++)
        s = submit(`echo line${i}`, 9230.95 + i / 10000);
    const bottom = rows(s).join("\n");
    if (bottom.includes("line0"))
        fail(`line0 was meant to have scrolled off: ${JSON.stringify(rows(s))}`);

    press(KEY.PAGE_UP, SHIFT);
    press(KEY.PAGE_UP, SHIFT);
    run(9230.96);
    s = descriptor(addr);
    if (row(s, 0) !== "line0")
        fail(`Shift+PageUp put ${JSON.stringify(row(s, 0))} on top, expected line0`);
    if (s.cursor_on !== 0)
        fail("the cursor is drawn over the scrollback");

    // Back down again, onto exactly the screen that was left behind.
    press(KEY.PAGE_DOWN, SHIFT);
    press(KEY.PAGE_DOWN, SHIFT);
    run(9230.97);
    s = descriptor(addr);
    if (rows(s).join("\n") !== bottom)
        fail(`Shift+PageDown did not restore the screen: ${JSON.stringify(rows(s))}`);
    if (!s.cursor_on)
        fail("the cursor did not come back with the live screen");

    // And any other key is the way back, the keystroke itself still landing.
    press(KEY.PAGE_UP, SHIFT);
    run(9230.98);
    type("x");
    run(9230.99);
    s = descriptor(addr);
    if (row(s, s.cursor_y) !== `${prompt()} x`)
        fail(`typing did not leave the scrollback: ${JSON.stringify(rows(s))}`);
    press("c".codePointAt(0), CTRL);
    run(9230.995);

    // A program holding the screen keeps the chord: less pages its own grid,
    // and the console's history is not the one on screen.
    s = submit("clear", 9230.996);
    type("less /README");
    press(KEY.ENTER);
    run(9230.997);
    press(KEY.PAGE_UP, SHIFT);
    run(9230.998);
    s = descriptor(addr);
    if (!rows(s).some((line) => line.includes("/README") && line.includes("q quits")))
        fail(`Shift+PageUp took the screen from less: ${JSON.stringify(rows(s))}`);
    press("q".codePointAt(0));
    run(9230.999);
    s = submit("echo alive", 9230.9995);
    if (!rows(s).includes("alive"))
        fail(`the shell did not survive the pager: ${JSON.stringify(rows(s))}`);

    // From here to `exit`, every case takes a worker away — and the shell is
    // one of the processes holding one, so each of them kills it and init
    // replaces what died (Concept.md §4). That bound is `RESPAWN_TRIES` deaths
    // inside `RESPAWN_FLOOR_MS` of *scheduler* time (src/user/boot.cpp), and
    // scheduler time here is whatever literal `run()` is passed. So the blocks
    // below are a second or more apart on that clock rather than a millisecond,
    // which is what keeps a shell that died from counting as a crash loop.
    // Anything inserted here needs the same spacing, or the session ends at
    // "the shell will not stay up" before `exit 7` is reached.

    // A worker taken away with a step still in it. `dropWorkers` is a host
    // letting go of every worker where `broke()` lets go of one link, and
    // either way the process has to be *failed* by whoever killed it: an
    // unanswered request the kernel is parked on is answered by nothing else.
    //
    // The shell holds a worker too now, so it goes with them and init starts
    // another — which is what makes this the strongest form of the case. Before
    // T8 a missed failure was a prompt that never came back; it is a whole
    // session that never comes back now, since the shell parked on the step it
    // was owed is the shell nobody will replace.
    s = submit("clear", 9075);
    net.hold();
    type("spin");
    press(KEY.ENTER);
    if (run(9075.1) !== -1)
        fail("a spinning process left the kernel with work to do");
    if (others() !== 1)
        fail("spin did not reach an instance");

    net.proc.dropWorkers();
    if (run(9075.2) !== -1)
        fail("dropping the workers left the kernel with work to do");
    s = descriptor(addr);
    if (!rows(s).some((line) => line.startsWith("braam: the shell died")))
        fail(`dropping the workers said ${JSON.stringify(rows(s))}`);
    // A bare prompt, not `spin`'s status: the shell that would have printed it
    // died in the same breath, and its replacement has no line to report.
    if (row(s, s.cursor_y) !== prompt())
        fail(`a dropped worker left ${row(s, s.cursor_y)}, expected a bare prompt`);
    if (others() !== 0)
        fail(`${others()} instances outlived the workers holding them`);
    net.release();

    // And the replacement got a worker, because `dropWorkers` lets go of the
    // workers it has rather than of the ability to make one: a host that can
    // still hire answers the next `exec` with one. There is no latch behind
    // that any more — the kernel is what paces the asking (Concept.md §4).
    s = submit("clear", 9075.3);
    s = submit("echo alive", 9075.4);
    if (!rows(s).includes("alive"))
        fail(`the shell after a dropped worker printed ${JSON.stringify(rows(s))}`);

    // The shell's own worker going away, which is the thing T8 risks rather
    // than a stand-in for it: init notices its child *died* rather than exited
    // and starts another (Concept.md §4). Killed from the host, since `kill`
    // refuses anything that is not a child of the caller.
    //
    // The shell's pid is init's — it runs inside init's task rather than a job
    // of its own — and /proc says so: a cwd is what only a program has. The tty
    // pump is spawned first, so init is 2.
    s = submit("clear", 11076);
    s = submit("cat /proc/2", 11076.1);
    if (!rows(s).some((line) => line.startsWith("name   init")))
        fail(`/proc/2 is not init: ${JSON.stringify(rows(s))}`);
    // Both memory figures are here, where a column would have been uniform: what
    // the instance holds, and the ceiling the kernel gave it.
    if (!rows(s).some((line) => /^mem    \d+$/.test(line)))
        fail(`/proc/2 does not report its memory: ${JSON.stringify(rows(s))}`);
    if (!rows(s).includes(`cap    ${256 * 65536}`))
        fail(`/proc/2 does not report its cap: ${JSON.stringify(rows(s))}`);
    if (!rows(s).some((line) => line.startsWith("cwd    /home")))
        fail(`/proc/2 has no cwd, so no process is there: ${JSON.stringify(rows(s))}`);

    // Killed with state on it: a job in its table, and a foreground armed. A
    // pipeline rather than one command, because a single child clears the
    // console on its way out and a stage does not — the shell clears it after
    // collecting, and that is the line the dead shell never reaches. It is
    // parked on Sys::Wait meanwhile, so it learns nothing until the stage it is
    // waiting for finishes, which is what the second run() is for.
    s = submit("clear", 11076.2);
    s = submit("sleep 60000 &", 11076.3);
    if (!rows(s).some((line) => /^\[\d+\] \d+$/.test(line)))
        fail(`the doomed shell did not announce the job: ${JSON.stringify(rows(s))}`);
    type("sleep 1 | wc");
    press(KEY.ENTER);
    run(11076.4);

    const live = net.proc.live();
    net.terminated.length = 0;
    net.proc.kill(2);
    if (net.proc.live() !== live - 1)
        fail("pid 2 is not the shell, so the case below would assert nothing");
    // The shell is a process like any other, so the kill reaches a worker
    // rather than merely dropping the record. It is parked on Sys::Wait with no
    // step outstanding, so this is the branch that has nothing to fail.
    if (net.terminated.length !== 1)
        fail(`killing the shell terminated ${net.terminated.length} workers, expected 1`);
    run(11078); // the timer fires, the child exits, and the shell steps to collect it

    s = descriptor(addr);
    if (!rows(s).some((line) => line.startsWith("braam: the shell died")))
        fail(`a shell whose worker went away said ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`the replacement shell left ${row(s, s.cursor_y)}, expected a bare prompt`);
    if (net.proc.live() !== 1)
        fail(`${net.proc.live()} processes after the replacement, expected the shell alone`);

    // ^C at its prompt, which is what init clearing the console foreground
    // buys, and it has to be the *first* thing the replacement is asked to do:
    // the pump cancels whatever is in front rather than asking whether it is
    // still alive, and any command run here would clear the stale set on its
    // way out. Without the clear the ^C is swallowed and the typed line
    // survives it, which is what the second half asserts.
    type("junk");
    press("c".codePointAt(0), CTRL);
    run(11079);
    s = descriptor(addr);
    if (!rows(s).includes(prompt(130)))
        fail(`^C on the replacement left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);
    s = submit("echo clean", 11079.1);
    if (!rows(s).includes("clean"))
        fail(`the abandoned line was still in the editor: ${JSON.stringify(rows(s))}`);

    // A fresh shell, not the one that died: its table is empty, and what the
    // dead one backgrounded went with it — a process's children are cancelled
    // by its destructor.
    s = submit("clear", 11079.2);
    s = submit("jobs", 11079.3);
    if (rows(s).some((line) => line.includes("sleep")))
        fail(`the replacement inherited a job: ${JSON.stringify(rows(s))}`);

    // And the other half of the console: the replacement arming a foreground of
    // its own, and taking the screen back from a full-screen child.
    type("sleep 60000");
    press(KEY.ENTER);
    run(11079.6);
    press("c".codePointAt(0), CTRL);
    run(11079.7);
    s = descriptor(addr);
    if (!rows(s).includes(prompt(130)))
        fail(`^C on the replacement's foreground left ${JSON.stringify(rows(s))}`);

    s = submit("clear", 11079.8);
    type("less /README");
    press(KEY.ENTER);
    run(11079.9);
    s = descriptor(addr);
    if (!rows(s).some((line) => line.includes("/README") && line.includes("q quits")))
        fail(`less under the replacement painted ${JSON.stringify(rows(s))}`);
    press("q".codePointAt(0));
    run(11080);
    s = descriptor(addr);
    if (row(s, s.cursor_y) !== prompt())
        fail(`less did not give the replacement its screen back: ${JSON.stringify(rows(s))}`);

    // The two ways a worker is not to be had, last of all, because both leave
    // the session in a state nothing after them should have to work around.
    //
    // A worker that is made and never loads its script. The kernel learns at
    // the first step, so the process reads as a crash — and nothing is latched
    // off by it: whether procworker.js loads is a question the host answers
    // afresh every time it is asked, which is what lets a host that recovers be
    // noticed (Concept.md §4).
    //
    // The pool is emptied by a *pipeline* rather than by `dropWorkers`, which
    // would take the shell's worker with it and make this a case about init.
    // One stage takes the last idle worker and the second has to hire, so the
    // second is the one that gets the broken link — and the shell keeps the
    // good worker it was already holding. The screen is cleared first, since
    // `clear` is a program too and would otherwise be a third claimant.
    s = submit("clear", 13086);
    if (net.proc.pooled() !== 1)
        fail(`the pool holds ${net.proc.pooled()} workers, expected one before the break`);
    net.broken = true;
    s = submit("echo hi | cat", 13087);
    if (!rows(s).some((line) => line.startsWith("braam: /bin/cat: crashed")))
        fail(`a worker that never loaded printed ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== prompt(132))
        fail(`a crashed process left ${row(s, s.cursor_y)}, expected ${prompt(132)}`);
    if (others() !== 0)
        fail(`${others()} instances outlived a pipeline with a broken stage`);

    // And the next pipeline hires again and crashes the same way. That is the
    // latch's absence stated as an assertion: a host whose workers are born
    // broken is asked afresh rather than written off, and the cost of it is one
    // dead worker per hire until it recovers. A pipeline rather than a command,
    // because `echo` gave its own worker back to the pool when it exited and a
    // single command would take that one and never hire.
    const made = net.links.length;
    s = submit("echo hi | cat", 13089);
    if (row(s, s.cursor_y) !== prompt(132))
        fail(`a second pipeline after a broken worker left ${row(s, s.cursor_y)}`);
    if (net.links.length === made)
        fail("a broken worker stopped the next pipeline from hiring one");
    net.broken = false;

    // Where a worker cannot be made at all, the spawn *waits* for one: 10 ms,
    // then 20, 50, and so on to a second, saying so each time, until the host
    // can give one (Concept.md §4). There is no second place to run a process,
    // so this is the whole of what a host without workers gets.
    //
    // The shell is one of them and dies here for the last time: the drop takes
    // the worker it is holding, and the `exec` init answers with is the one
    // that waits. Everything after this line depends on it coming back.
    s = submit("clear", 13089.4);
    net.workers = false;
    net.proc.dropWorkers();
    net.bound.length = 0; // nothing may bind one from here until there is one

    // The kernel learns its shell is gone when it next tries to step it, and at
    // a prompt that is the next key: nothing is outstanding to be failed, since
    // the shell is parked on `key_read` and the *kernel* is holding that. So one
    // keystroke is spent provoking it, and it goes with the shell it reached.
    press("x".codePointAt(0));
    run(13089.5);
    s = descriptor(addr);
    if (!rows(s).some((line) => line.startsWith("braam: the shell died")))
        fail(`losing the workers said ${JSON.stringify(rows(s))}`);

    const WAIT_LINE = "braam: /bin/sh: no worker, retrying";
    const waiting = (d) => rows(d).filter((line) => line === WAIT_LINE).length;
    if (waiting(s) !== 1)
        fail(`a host with no worker said ${JSON.stringify(rows(s))}`);

    // The backoff, on the clock the driver owns: 10 ms after the first refusal,
    // then 20 after the second. Nothing else is running, so each is a line.
    run(13099.6);
    run(13119.7);
    s = descriptor(addr);
    if (waiting(s) !== 3)
        fail(`the spawn backed off ${waiting(s)} times, expected three`);
    if (net.bound.length !== 0)
        fail("a spawn with no worker to be had bound one anyway");

    // And it is a wait rather than a failure: the host finds a worker, the
    // shell that has been waiting for one starts, and the session is back.
    net.workers = true;
    run(13169.8);
    s = descriptor(addr);
    if (row(s, s.cursor_y) !== prompt())
        fail(`a shell that waited for a worker left ${row(s, s.cursor_y)}`);
    s = submit("echo back", 13171);
    if (!rows(s).includes("back"))
        fail(`the shell after a wait printed ${JSON.stringify(rows(s))}`);

    // Globbing over the real store, and `case` beside it: both go through
    // match.cpp, which is why they land together. The unit suite has the
    // matcher; what it cannot see is a listing reaching a real argv. Late in
    // the file, since each of these spends pids the ps cases above count on.
    submit("mkdir /home/g", 13172);
    submit("mkdir /home/g/sub", 13172.1);
    submit("touch /home/g/aaa /home/g/bb", 13172.2);
    submit("echo x > /home/g/.dot", 13172.3);

    let gt = 13173;
    const gshows = (line, want) => {
        submit("clear", (gt += 0.01));
        const got = output(submit(line, (gt += 0.01))).join("|");
        if (got !== want)
            fail(`\`${line}\` printed ${JSON.stringify(got)}, expected ${JSON.stringify(want)}`);
    };

    gshows("echo /bin/l*", "/bin/less /bin/ls");
    gshows("echo /home/g/*", "/home/g/aaa /home/g/bb /home/g/sub");
    gshows("echo /home/g/.*", "/home/g/.dot"); // a leading dot is asked for
    gshows("echo /home/g/?b", "/home/g/bb");
    gshows("echo /home/g/[ab]*", "/home/g/aaa /home/g/bb");
    gshows("echo /home/g/*/", "/home/g/sub/"); // a trailing slash means dirs
    gshows("echo g*", "g"); // relative: the walk lists the cwd
    gshows("echo '/bin/l*'", "/bin/l*"); // quoted: the star is itself
    gshows("echo /bin/nosuch*", "/bin/nosuch*"); // no match: the word as written
    gshows("for f in /home/g/[ab]*; do echo $f; done", "/home/g/aaa|/home/g/bb");
    gshows("v=/home/g/b*; echo $v", "/home/g/bb"); // a star out of a variable

    gshows("case hi in h*) echo yes;; *) echo no;; esac", "yes");
    gshows("case hi in 'h*') echo yes;; *) echo no;; esac", "no"); // quoted pattern
    gshows("case hi in a|hi) echo two;; esac", "two");
    gshows("case hi in a) echo no;; esac", ""); // no arm ran
    gshows("case /home/g/bb in */bb) echo path;; esac", "path");
    submit("rm -r /home/g", (gt += 0.01));

    // Command substitution: a pipe the shell drains itself. The unit suite has
    // the hook against a canned callback; what it cannot reach is a real
    // command writing down a real pipe.
    submit("mkdir /home/c", (gt += 0.01));
    // Three copies of a 2,359-byte file: more than the eight writes a pipe
    // holds, so the drain has to be running before the wait or this hangs.
    submit("cat /share/help /share/help /share/help > /home/c/big", (gt += 0.01));

    const cshows = (line, want) => {
        submit("clear", (gt += 0.005));
        const got = output(submit(line, (gt += 0.005))).join("|");
        if (got !== want)
            fail(`\`${line}\` printed ${JSON.stringify(got)}, expected ${JSON.stringify(want)}`);
    };

    cshows("echo $(echo hi)", "hi");
    cshows("echo `echo tick`", "tick"); // the backtick form
    cshows("echo a$(echo b)c", "abc");
    cshows("echo $(echo a b)", "a b"); // splits, then echo rejoins
    // A `$(…)` inside quotes quotes independently, and the captured spacing
    // survives because the field is not split.
    cshows("echo \"$(echo 'a   b')\"", "a   b");
    cshows("echo $(echo 'a   b')", "a b"); // unquoted: split, then rejoined
    cshows("echo '$(echo no)'", "$(echo no)"); // single quotes: as typed
    cshows("echo $(echo one; echo two)", "one two"); // a list, in order
    cshows("echo $(echo $(echo deep))", "deep"); // nested
    cshows("x=$(ls /bin | grep less); echo $x", "less"); // through a real pipeline
    cshows("echo $(echo hi | wc)", "1 1 3");
    cshows("echo $(pwd)/x", "/home/x"); // no trailing newline in the value
    cshows("echo $(jobs)done", "done"); // a builtin down the same pipe
    cshows("echo $(nosuchcmd) after", "braam: nosuchcmd: not found|after");
    cshows("for f in $(echo p q); do echo $f; done", "p|q");
    cshows("case $(echo hi) in h*) echo yes;; esac", "yes");
    // The many-writes case: 7,077 bytes is fourteen chunks against eight
    // slots, so without drain-before-wait this one hangs rather than fails.
    cshows("x=$(cat /home/c/big); echo \"$x\" | wc", "123 1191 7077");
    submit("rm -r /home/c", (gt += 0.01));

    // exit ends the shell, and nothing runs after it — not even the rest of
    // its own line, which is Flow::Exit end to end. Last, for that reason.
    s = submit("clear", 14097);
    s = submit("echo before; exit 7; echo never", 14098);
    if (!rows(s).includes("before"))
        fail(`the list before exit printed ${JSON.stringify(rows(s))}`);
    if (rows(s).includes("never"))
        fail("a command ran after exit on the same line");
    if (!rows(s).some((line) => line.startsWith("braam: the shell exited")))
        fail(`exit said nothing: ${JSON.stringify(rows(s))}`);
    s = submit("echo after", 14099);
    if (rows(s).includes("after"))
        fail("a command ran after the shell exited");

    console.log(`smoke ok: ${got_imports.length} imports, ${got_exports.length} exports`);
} else {
    const failures = instance.exports.run_tests();
    if (failures !== 0)
        fail(`${failures} check(s) failed`);
}

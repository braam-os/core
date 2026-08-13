// Headless driver for kernel.wasm and tests.wasm. Node stands in for the
// browser: instantiating a freestanding module needs nothing browser-specific,
// and test/fakefs.mjs stands in for OPFS.

import { readFileSync } from "node:fs";
import { basename } from "node:path";

import { FakeStore, makeFakeImports } from "./fakefs.mjs";
import { FakeNet, makeFakeSvc } from "./fakesvc.mjs";

function usage() {
    console.error("usage: run.mjs --kernel <wasm> [<bundle.bin>] | --tests <wasm>");
    process.exit(2);
}

const [mode, file, bundle] = process.argv.slice(2);
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

// Named keys, from the enum in src/kernel/key.h — keep the two in step.
const NAMED = 0x110000;
const KEY = { ENTER: NAMED, BACKSPACE: NAMED + 1, UP: NAMED + 6, HOME: NAMED + 10 };
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

if (mode === "--kernel") {
    // The import and export surface is the ABI; drift is a bug, and an
    // unexpected import means a libc dependency crept in.
    const want_imports = ["host.fs", "host.fs_sync", "host.log", "host.now", "host.present",
                          "host.svc"];
    const want_exports = ["init", "key", "memory", "ref", "resize", "tick", "wake"];
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

    // init spawns the shell and nothing else. The first tick mounts the
    // filesystem, draws the prompt and parks the shell on the keyboard, so
    // there is nothing left pending.
    if (instance.exports.tick(0) !== -1)
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

    submit("clear", 1045); // the programs need more than the whole grid
    addr = instance.exports.resize(60, 40);
    if (addr === 0)
        fail("the resize before help failed");
    s = submit("help", 1050);
    for (const name of ["cat", "cd", "chat", "clear", "curl", "date", "df", "echo", "export",
                        "false", "grep", "head", "help", "import", "ls", "mkdir", "mount",
                        "pbcopy", "pbpaste", "pwd", "rm", "sleep", "tail", "touch", "true",
                        "version", "wc"])
        if (!rows(s).some((line) => line.startsWith(`  ${name} `)))
            fail(`help did not list ${name}: ${JSON.stringify(rows(s))}`);

    addr = instance.exports.resize(60, 16);
    if (addr === 0)
        fail("the resize after help failed");

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
    // program registry as a filesystem, and grep filters the listing, both
    // running at once over a bounded pipe. `clear` first, so the rows below
    // are the pipeline's and nothing else's.
    addr = instance.exports.resize(60, 16);
    if (addr === 0)
        fail("the resize before the pipeline failed");
    press("c".codePointAt(0), CTRL); // the "hi" typed above is still pending
    s = submit("clear", 1130);
    s = submit("ls /bin | grep hel", 1140);
    const listed = rows(s).filter((line) => line && !line.includes("$"));
    if (listed.join() !== "help")
        fail(`ls /bin | grep hel printed ${JSON.stringify(listed)}, expected ["help"]`);
    if (row(s, s.cursor_y) !== "$")
        fail(`a pipeline that matched left ${row(s, s.cursor_y)}, expected $`);

    // The status of a pipeline is its last command's: grep reports 1 when
    // nothing matched, and quote removal reaches argv on the way in.
    s = submit("ls /bin | grep zzz", 1150);
    if (!rows(s).includes("[1] $"))
        fail(`an empty pipeline left ${row(s, s.cursor_y)}, expected [1] $`);
    s = submit("echo 'a b' | wc", 1160);
    if (!rows(s).includes("1 2 4"))
        fail(`echo 'a b' | wc printed ${JSON.stringify(rows(s))}, expected 1 2 4`);

    // M5: the shell starts in /home, which is where a redirection lands.
    s = submit("clear", 1165);
    s = submit("pwd", 1166);
    if (!rows(s).includes("/home"))
        fail(`pwd printed ${JSON.stringify(rows(s))}, expected /home`);

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
    s = submit("echo hi > /bin/echo", 1174);
    if (!rows(s).some((line) => line.startsWith("braam: /bin/echo: ")))
        fail(`a read-only redirection said nothing: ${JSON.stringify(rows(s))}`);
    if (!rows(s).includes("[1] $"))
        fail(`a refused redirection left ${row(s, s.cursor_y)}, expected [1] $`);

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

    // The boot archive is mounted read-only on /usr, unpacked from the file
    // tools/pack.py wrote at the end of the build.
    if (bundle) {
        s = submit("clear", 1183);
        s = submit("cat /usr/share/motd", 1184);
        if (!rows(s).some((line) => line.startsWith("braam —")))
            fail(`/usr/share/motd did not read back: ${JSON.stringify(rows(s))}`);
        s = submit("clear", 1185);
        s = submit("ls /usr", 1186);
        if (!rows(s).includes("share/"))
            fail(`/usr did not list its directories: ${JSON.stringify(rows(s))}`);
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
    if (instance.exports.tick(1190) !== 5000)
        fail("the pipeline did not park on the timer");
    press("c".codePointAt(0), CTRL);
    if (instance.exports.tick(1200) !== -1)
        fail("^C left the pipeline's timer armed");
    s = descriptor(addr);
    if (!rows(s).includes("[130] $"))
        fail(`^C on a pipeline left ${row(s, s.cursor_y)}, expected [130] $`);

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
    if (instance.exports.tick(1220) !== -1)
        fail("a deferred storage reply left something scheduled");
    if (store.held.length === 0)
        fail("ls issued no storage request");
    store.defer = false;
    while (store.held.length)
        instance.exports.wake(store.held.shift(), 0, 0);
    instance.exports.tick(1230);
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
    instance.exports.tick(2000);
    submit("clear", 2005); // the second boot banner is still on the grid
    s = submit("cat notes", 2010);
    const survived = rows(s).filter((line) => line && !line.includes("$"));
    if (survived.join(",") !== "one,two")
        fail(`the file did not survive the reload: ${JSON.stringify(survived)}`);

    // M5, third criterion: with no OPFS the system still boots, on MemFs, and
    // says so rather than letting the user find out by losing a file.
    store.reset();
    store.opfs = false;
    store.sync = false;
    instantiate();
    instance.exports.init(0);
    addr = instance.exports.resize(60, 16);
    instance.exports.tick(3000);
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

    // M6, second criterion: a chat client over a WebSocket. The fake loops a
    // lone socket back to itself, so one client is a whole conversation; the
    // receiver is a job of its own, which is what makes the reply arrive while
    // the program is parked on the keyboard.
    s = submit("clear", 3030);
    type("chat ws://loop me");
    press(KEY.ENTER);
    instance.exports.tick(3031);
    if (net.sockets.length !== 1)
        fail(`chat opened ${net.sockets.length} sockets, expected 1`);
    s = submit("hello", 3032);
    if (!rows(s).includes("me: hello"))
        fail(`the chat message did not come back: ${JSON.stringify(rows(s))}`);

    // ^C leaves the socket dropped and the prompt back, with the receiver job
    // taken down by the destructor in its parent's frame.
    press("c".codePointAt(0), CTRL);
    if (instance.exports.tick(3033) !== -1)
        fail("^C left the chat receiver scheduled");
    s = descriptor(addr);
    if (!rows(s).includes("[130] $"))
        fail(`^C on chat left ${row(s, s.cursor_y)}, expected [130] $`);
    if (!net.sockets[0].closed)
        fail("chat did not drop its socket on the way out");

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
    if (instance.exports.tick(3056) !== -1)
        fail("pbpaste did not park on the paste");
    s = descriptor(addr);
    if (!rows(s).some((line) => line.startsWith("pbpaste: press ")))
        fail(`pbpaste did not ask for a gesture: ${JSON.stringify(rows(s))}`);
    if (!net.paste("pasted by hand"))
        fail("pbpaste was not waiting for a paste");
    instance.exports.tick(3057);
    s = descriptor(addr);
    if (!rows(s).includes("pasted by hand"))
        fail(`the paste never arrived: ${JSON.stringify(rows(s))}`);

    // ^C while it waits gets the prompt back, and leaves the arming behind it
    // to be reaped when the paste finally lands.
    type("pbpaste");
    press(KEY.ENTER);
    instance.exports.tick(3058);
    press("c".codePointAt(0), CTRL);
    if (instance.exports.tick(3059) !== -1)
        fail("^C left the paste wait scheduled");
    s = descriptor(addr);
    if (!rows(s).includes("[130] $"))
        fail(`^C on pbpaste left ${row(s, s.cursor_y)}, expected [130] $`);
    net.paste("too late");
    instance.exports.tick(3060);
    net.clipDenied = false;

    s = submit("clear", 3053);
    s = submit("date -u", 3054);
    if (!rows(s).some((line) => /^\w\w\w \w\w\w \d\d \d\d:\d\d:\d\d \+0000 \d{4}$/.test(line)))
        fail(`date printed ${JSON.stringify(rows(s))}`);

    console.log(`smoke ok: ${got_imports.length} imports, ${got_exports.length} exports`);
} else {
    const failures = instance.exports.run_tests();
    if (failures !== 0)
        fail(`${failures} check(s) failed`);
}

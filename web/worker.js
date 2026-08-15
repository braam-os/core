// The kernel lives here. The main thread only relays events and pixels
// (Concept.md §3): OPFS sync handles and OffscreenCanvas both need a worker.

import { makeFsImports, openStore } from "./fs.js";
import { Memory, makeImports } from "./host.js";
import { makeProc } from "./proc.js";
import { Renderer } from "./render.js";
import { makeSvcImport } from "./svc.js";

const mem = new Memory();

let renderer = null;
let pending = null; // a canvas or viewport that arrived before the kernel did

// What an embedder may choose: where the module and the bundle live, and how
// the renderer draws. The defaults are the files beside this one, so a page
// that wants none of this posts nothing (web/braam.js).
let options = {};

// navigator.storage.persist() is main-thread only (Concept.md §A.2), so the
// page calls it and posts the answer down. Boot waits for it rather than
// guessing, since `df` reporting the wrong durability is worse than a tick of
// delay — but only briefly: the page sends a provisional answer when the
// browser is slow to decide, and the real one after it. The second answer
// corrects the store rather than boot.
let persisted = null;
const persistedKnown = new Promise((resolve) => {
    persisted = resolve;
});

let store = null;

// The isolated processes, so that a dispose can let go of their workers.
let proc = null;

// The same handshake for the embedder's options, which decide where the module
// is fetched from and therefore cannot be applied after boot has started.
let configured = null;
const configuredKnown = new Promise((resolve) => {
    configured = resolve;
});

function emit(kind, text) {
    self.postMessage({ kind, text });
}

// The clipboard, the file picker and the download need the DOM, so the page
// does them and answers by id. Everything else in svc.js happens right here.
let next_relay = 1;
const relays = new Map();

function relay(msg) {
    return new Promise((resolve, reject) => {
        const id = next_relay++;
        relays.set(id, { resolve, reject });
        self.postMessage({ kind: "svc", id, ...msg });
    });
}

async function boot() {
    const url = new URL(options.wasmUrl || "./kernel.wasm", import.meta.url);
    const bundle = new URL(options.bundleUrl || "./bundle.bin", import.meta.url);
    store = await openStore(bundle, await persistedKnown);

    // A reply arrives on a promise, so it is never on the stack of the tick
    // that issued the request; pumping from here is what gets the resumed task
    // moving when nothing else is scheduled.
    const fs = makeFsImports(mem, store, (token) => {
        self.kernel.wake(token >>> 0, 0, 0);
        pump();
    });

    // Isolated processes live here too (Concept.md §4): tier 2 as instances in
    // this worker, tier 3 in workers of their own. It is handed a getter rather
    // than the exports, because the kernel does not exist yet.
    //
    // A tier-2 step runs off the kernel's stack, which a microtask is enough
    // for. Now and then it takes the slower route instead: a process in a tight
    // syscall loop would otherwise chain microtasks without the worker ever
    // painting. A tier-3 step is a message and is already an event-loop turn.
    let steps = 0;
    proc = makeProc(mem, () => self.kernel, (drain) => {
        if (++steps % 64)
            queueMicrotask(drain);
        else
            setTimeout(drain, 0);
    }, () => new Worker(new URL(options.procWorkerUrl || "./procworker.js", import.meta.url),
                        { type: "module" }),
       () => performance.now());

    // The same rule as storage: a reply must reach the kernel on a promise,
    // never inside the import call that asked for it.
    const svc = makeSvcImport(mem, (slot, obj) => self.kernel.ref(slot >>> 0, obj), relay,
        (token) => {
            self.kernel.wake(token >>> 0, 0, 0);
            pump();
        }, proc);

    const imports = makeImports(mem, (text) => emit("log", text), (x, y, w, h) => {
        if (renderer)
            renderer.present(x, y, w, h);
    }, fs, svc);

    // Streaming needs an application/wasm content type; not every static host
    // sets one, so fall back to a buffered instantiate.
    let instance;
    try {
        ({ instance } = await WebAssembly.instantiateStreaming(fetch(url), imports));
    } catch {
        const buf = await (await fetch(url)).arrayBuffer();
        ({ instance } = await WebAssembly.instantiate(buf, imports));
    }

    mem.bind(instance.exports.memory);

    // 0 means "use the linker's __heap_base"; an isolated process (M8) is
    // handed a real base instead.
    instance.exports.init(0);

    self.kernel = instance.exports;

    // The canvas may have arrived before the module finished compiling.
    if (pending) {
        const { canvas, viewport } = pending;
        pending = null;
        if (canvas)
            attach(canvas);
        if (viewport)
            fit(viewport);
    }
    pump();
}

function attach(canvas) {
    renderer = new Renderer(canvas, mem, options);
}

// A mouse selection is the page's gesture and the renderer's highlight, and the
// kernel is told nothing about either (Concept.md §3.5). The text crosses back
// when it settles, because only the page can reach the clipboard.
function deselect() {
    if (renderer && renderer.clear())
        self.postMessage({ kind: "selection", text: "" });
}

// The worker owns the font, so it owns the geometry: the page reports a box in
// device pixels and reads back whatever the kernel accepted.
function fit({ width, height, dpr }) {
    if (!renderer)
        return;
    deselect();
    const { cols, rows } = renderer.fit(width, height, dpr);
    const info = self.kernel.resize(cols, rows);
    if (info === 0) {
        emit("error", `braam: no memory for a ${cols}x${rows} screen`);
        return;
    }
    renderer.attach(info);
}

// The event loop is the scheduler (Concept.md §2.1). tick() drains the ready
// queue and says how long until it next needs to run; -1 means idle.
let timer = null;

function pump() {
    if (timer !== null) {
        clearTimeout(timer);
        timer = null;
    }
    const delay = self.kernel.tick(performance.now());
    if (delay >= 0)
        timer = setTimeout(pump, delay);
}

// A paste is a run of keystrokes (web/keys.js), and a run is longer than the
// key ring — so it is fed at the rate the console drains it rather than all at
// once, which would drop everything past the ring's last slot. key() says
// whether it took the keystroke; a refusal means the ring is full, and the rest
// of the run waits for the tick that empties it. A second paste joins the queue
// instead of displacing it; a key typed while one is being fed goes straight in
// ahead of the rest, since ^C must not wait behind a paste.
let pasting = null; // { codes, at }, the run being fed

function feed() {
    while (pasting.at < pasting.codes.length && self.kernel.key(pasting.codes[pasting.at] >>> 0, 0))
        pasting.at++;
    pump();
    if (pasting.at < pasting.codes.length)
        setTimeout(feed, 0);
    else
        pasting = null;
}

function type(codes) {
    if (pasting) {
        pasting.codes = pasting.codes.slice(pasting.at).concat(codes);
        pasting.at = 0;
        return; // the feed already waiting on a turn will take it
    }
    pasting = { codes, at: 0 };
    feed();
}

// Events reach a suspended task as a wake token, never as a return value
// (Concept.md §2.2). Every one of them pumps: when nothing is sleeping there is
// no timer armed, so queued work would otherwise sit there forever.
self.onmessage = ({ data }) => {
    if (!data)
        return;

    // Boot itself waits on these two, so they are answered before the kernel
    // exists. `options` must arrive first of all, since boot reads it.
    if (data.kind === "options") {
        options = data.options || {};
        configured(true);
        return;
    }

    if (data.kind === "persisted") {
        persisted(!!data.value);
        if (store)
            store.persisted = !!data.value;
        return;
    }

    // The page is letting go, and answered before boot has finished as well as
    // after: a tier-3 process is a worker of this one, and one spinning in a
    // loop is a core burning until somebody says stop.
    if (data.kind === "dispose") {
        if (proc)
            proc.shutdown();
        self.close();
        return;
    }

    if (data.kind === "svc-reply") {
        const waiting = relays.get(data.id);
        if (!waiting)
            return;
        relays.delete(data.id);
        if (data.error)
            waiting.reject({ braam: data.error });
        else
            waiting.resolve(data.value);
        return;
    }

    if (!self.kernel) {
        pending = pending || {};
        if (data.kind === "canvas")
            pending.canvas = data.canvas;
        else if (data.kind === "viewport")
            pending.viewport = data;
        return;
    }

    switch (data.kind) {
    case "canvas":
        attach(data.canvas);
        break;
    case "viewport":
        fit(data);
        pump();
        break;
    case "key":
        deselect();
        self.kernel.key(data.code >>> 0, data.mods >>> 0);
        pump();
        break;
    case "select":
        if (renderer) {
            renderer.select(data.phase, data.x, data.y);
            if (data.phase === "end")
                self.postMessage({ kind: "selection", text: renderer.text() });
        }
        break;
    case "selectall":
        if (renderer) {
            renderer.all();
            self.postMessage({ kind: "selection", text: renderer.text() });
        }
        break;
    case "paste":
        deselect();
        type(data.codes || []);
        break;
    case "deselect":
        deselect();
        break;
    case "wake":
        self.kernel.wake(data.token >>> 0, data.ptr >>> 0, data.len >>> 0);
        pump();
        break;
    }
};

// Nothing is fetched until the embedder's options arrive, since they say what
// to fetch. web/braam.js posts them before anything else.
configuredKnown
    .then(boot)
    .catch((e) => emit("error", `boot failed: ${e && e.message ? e.message : e}`));

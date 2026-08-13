// The kernel lives here. The main thread only relays events and pixels
// (Concept.md §3): OPFS sync handles and OffscreenCanvas both need a worker.

import { makeFsImports, openStore } from "./fs.js";
import { Memory, makeImports } from "./host.js";
import { Renderer } from "./render.js";

const mem = new Memory();

let renderer = null;
let pending = null; // a canvas or viewport that arrived before the kernel did

// navigator.storage.persist() is main-thread only (Concept.md §A.2), so the
// page calls it and posts the answer down. Boot waits for it rather than
// guessing, since `df` reporting the wrong durability is worse than a tick of
// delay.
let persisted = null;
const persistedKnown = new Promise((resolve) => {
    persisted = resolve;
});

function emit(kind, text) {
    self.postMessage({ kind, text });
}

async function boot() {
    const url = new URL("./kernel.wasm", import.meta.url);
    const bundle = new URL("./bundle.bin", import.meta.url);
    const store = await openStore(bundle, await persistedKnown);

    // A reply arrives on a promise, so it is never on the stack of the tick
    // that issued the request; pumping from here is what gets the resumed task
    // moving when nothing else is scheduled.
    const fs = makeFsImports(mem, store, (token) => {
        self.kernel.wake(token >>> 0, 0, 0);
        pump();
    });

    const imports = makeImports(mem, (text) => emit("log", text), (x, y, w, h) => {
        if (renderer)
            renderer.present(x, y, w, h);
    }, fs);

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
    renderer = new Renderer(canvas, mem);
}

// The worker owns the font, so it owns the geometry: the page reports a box in
// device pixels and reads back whatever the kernel accepted.
function fit({ width, height, dpr }) {
    if (!renderer)
        return;
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

// Events reach a suspended task as a wake token, never as a return value
// (Concept.md §2.2). Every one of them pumps: when nothing is sleeping there is
// no timer armed, so queued work would otherwise sit there forever.
self.onmessage = ({ data }) => {
    if (!data)
        return;

    // Boot itself waits on this one, so it is answered before the kernel exists.
    if (data.kind === "persisted") {
        persisted(!!data.value);
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
        self.kernel.key(data.code >>> 0, data.mods >>> 0);
        pump();
        break;
    case "wake":
        self.kernel.wake(data.token >>> 0, data.ptr >>> 0, data.len >>> 0);
        pump();
        break;
    }
};

boot().catch((e) => emit("error", `boot failed: ${e && e.message ? e.message : e}`));

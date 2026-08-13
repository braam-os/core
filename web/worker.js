// The kernel lives here. The main thread only relays events and pixels
// (Concept.md §3): OPFS sync handles and OffscreenCanvas both need a worker.

import { Memory, makeImports } from "./host.js";

const mem = new Memory();

function emit(kind, text) {
    self.postMessage({ kind, text });
}

async function boot() {
    const url = new URL("./kernel.wasm", import.meta.url);
    const imports = makeImports(mem, (text) => emit("log", text));

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
    pump();
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
// (Concept.md §2.2).
self.onmessage = ({ data }) => {
    if (data && data.kind === "wake" && self.kernel) {
        self.kernel.wake(data.token >>> 0, data.ptr >>> 0, data.len >>> 0);
        pump();
    }
};

boot().catch((e) => emit("error", `boot failed: ${e && e.message ? e.message : e}`));

// The JS half of the kernel ABI. Every import is non-blocking; results come
// back through wake() (Concept.md §2.2). host.now is a sanctioned exception.

// memory.grow detaches the ArrayBuffer, which kills any cached view
// (Concept.md §8.4). All access goes through view(), which re-derives.
export class Memory {
    constructor() {
        this.memory = null;
        this._u8 = null;
    }

    bind(memory) {
        this.memory = memory;
        this._u8 = null;
    }

    view() {
        if (this._u8 === null || this._u8.byteLength === 0)
            this._u8 = new Uint8Array(this.memory.buffer);
        return this._u8;
    }

    str(ptr, len) {
        return new TextDecoder().decode(this.view().subarray(ptr, ptr + len));
    }
}

export function makeImports(mem, sink) {
    return {
        host: {
            log(ptr, len) {
                sink(mem.str(ptr, len));
            },
            now() {
                return performance.now();
            },
        },
    };
}

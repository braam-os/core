// A storage backend for the tests: the same wire format web/fs.js speaks, over
// a Map that outlives the kernel instance. That is what lets the smoke test
// prove M5's first criterion — write a file, reload the page, the file is still
// there — with a fresh WebAssembly.Instance standing in for the reload.
//
// Replies are synchronous here, called from inside the import. A browser can
// never do that, but the kernel cannot tell: wake() only queues a resumption,
// and the tick that issued the request drains the queue on its way out. Set
// `defer` to hold replies back and prove the parked path as well.

import { E, OP, Request, SYNC, packEntries, packInfo } from "../web/fs.js";

const O_CREATE = 4, O_TRUNC = 8;

function dirname(p) {
    const at = p.lastIndexOf("/");
    return at <= 0 ? "/" : p.slice(0, at);
}

export class FakeStore {
    constructor() {
        this.reset();
    }

    reset() {
        this.dirs = new Set(["/"]);
        this.files = new Map(); // path -> Uint8Array
        this.handles = [];      // index -> path, or null
        this.opfs = true;
        this.sync = true;
        this.persisted = false;
        this.quota = 10737418240;
        this.bundle = null;
        this.defer = false;
        this.held = [];         // tokens whose replies are being withheld
    }

    usage() {
        let n = 0;
        for (const data of this.files.values())
            n += data.length;
        return n;
    }

    // Everything the kernel would have found after a reload, and nothing the
    // instance held: handles do not survive one.
    reopen() {
        this.handles = [];
        this.held = [];
    }
}

// `mem` is the shim run.mjs supplies; `kernel` returns the live instance's
// exports, which change when the test re-instantiates.
export function makeFakeImports(mem, store, kernel) {
    function perform(r, op) {
        const path = op === OP.INFO || op === OP.BUNDLE ? "" : r.path();
        switch (op) {
        case OP.INFO:
            return r.write(packInfo({
                quota: store.quota,
                usage: store.usage(),
                opfs: store.opfs,
                sync: store.sync,
                fsaccess: false,
                persisted: store.persisted,
            }));

        case OP.BUNDLE:
            if (!store.bundle) {
                r.set("resultLo", 0);
                r.set("bufLen", 0);
                r.set("status", 0);
                return;
            }
            return r.write(store.bundle);

        case OP.OPEN: {
            const flags = r.get("flags");
            if (store.dirs.has(path))
                return r.fail(E.ISDIR);
            if (!store.files.has(path)) {
                if (!(flags & O_CREATE))
                    return r.fail(E.NOTFOUND);
                if (!store.dirs.has(dirname(path)))
                    return r.fail(E.NOTFOUND);
                store.files.set(path, new Uint8Array(0));
            } else if (flags & O_TRUNC) {
                store.files.set(path, new Uint8Array(0));
            }
            let slot = store.handles.indexOf(null);
            if (slot < 0)
                slot = store.handles.push(null) - 1;
            store.handles[slot] = path;
            return r.ok(slot);
        }

        case OP.STAT: {
            if (store.dirs.has(path)) {
                r.set("flags", 1);
                return r.ok(0, 0);
            }
            const data = store.files.get(path);
            if (!data)
                return r.fail(E.NOTFOUND);
            r.set("flags", 0);
            return r.ok(data.length, 0);
        }

        case OP.LIST: {
            if (!store.dirs.has(path))
                return r.fail(store.files.has(path) ? E.NOTDIR : E.NOTFOUND);
            const out = [];
            const under = path === "/" ? "/" : path + "/";
            for (const name of store.dirs)
                if (name !== "/" && dirname(name) === path)
                    out.push({ name: name.slice(under.length), dir: true, size: 0 });
            for (const [name, data] of store.files)
                if (dirname(name) === path)
                    out.push({ name: name.slice(under.length), dir: false, size: data.length });
            r.set("status", 0);
            return r.write(packEntries(out));
        }

        case OP.MKDIR:
            if (store.dirs.has(path) || store.files.has(path))
                return r.fail(E.EXISTS);
            if (!store.dirs.has(dirname(path)))
                return r.fail(E.NOTFOUND);
            store.dirs.add(path);
            return r.ok();

        case OP.REMOVE: {
            const recursive = (r.get("flags") & 1) !== 0;
            if (store.files.delete(path))
                return r.ok();
            if (!store.dirs.has(path))
                return r.fail(E.NOTFOUND);

            const under = path + "/";
            const kids = [...store.dirs, ...store.files.keys()]
                .filter((n) => n.startsWith(under));
            if (kids.length && !recursive)
                return r.fail(E.NOTEMPTY);
            for (const n of kids) {
                store.dirs.delete(n);
                store.files.delete(n);
            }
            store.dirs.delete(path);
            return r.ok();
        }

        default:
            return r.fail(E.UNSUPPORTED);
        }
    }

    return {
        fs(op, token, req) {
            perform(new Request(mem, req), op);
            if (store.defer)
                store.held.push(token);
            else
                kernel().wake(token, 0, 0);
        },

        fs_sync(op, handle, ptr, len, off) {
            const path = store.handles[handle];
            if (path === null || path === undefined)
                return -E.INVALID;
            const data = store.files.get(path);
            if (!data)
                return -E.NOTFOUND;

            switch (op) {
            case SYNC.READ: {
                if (off >= data.length)
                    return 0;
                const n = Math.min(len, data.length - off);
                mem.view().set(data.subarray(off, off + n), ptr);
                return n;
            }
            case SYNC.WRITE: {
                let out = data;
                if (off + len > data.length) {
                    out = new Uint8Array(off + len);
                    out.set(data);
                    store.files.set(path, out);
                }
                out.set(mem.view().slice(ptr, ptr + len), off);
                return len;
            }
            case SYNC.SIZE:
                return data.length;
            case SYNC.TRUNCATE: {
                const out = new Uint8Array(off);
                out.set(data.subarray(0, Math.min(off, data.length)));
                store.files.set(path, out);
                return 0;
            }
            case SYNC.FLUSH:
                return 0;
            case SYNC.CLOSE:
                store.handles[handle] = null;
                return 0;
            default:
                return -E.UNSUPPORTED;
            }
        },
    };
}

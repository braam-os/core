// A storage backend for the tests: the same wire format web/fs.js speaks, over
// a Map that outlives the kernel instance. That is what lets the smoke test
// prove M5's first criterion — write a file, reload the page, the file is still
// there — with a fresh WebAssembly.Instance standing in for the reload.
//
// Replies are synchronous here, called from inside the import. A browser can
// never do that, but the kernel cannot tell: wake() only queues a resumption,
// and the tick that issued the request drains the queue on its way out. Set
// `defer` to hold replies back and prove the parked path as well.

import {
    E, OP, Request, SYNC, installOps, linkBytes, linkTarget, packEntries, packInfo, packStat,
} from "../web/fs.js";

const O_CREATE = 4, O_TRUNC = 8;

// The clock writes are stamped from: deterministic, and behind fakesvc's frozen
// wall clock so a stamped file reads as the recent past.
const CLOCK_START = 1781900000000, CLOCK_STEP = 1000;

function dirname(p) {
    const at = p.lastIndexOf("/");
    return at <= 0 ? "/" : p.slice(0, at);
}

// OPFS walks a component at a time, so a file where a directory had to be is
// NOTDIR. This flat map has to say the same, or the VFS's link walk never runs.
function notDir(store, path) {
    for (let at = path.indexOf("/", 1); at > 0; at = path.indexOf("/", at + 1))
        if (store.files.has(path.slice(0, at)))
            return true;
    return false;
}

function removeTree(store, path) {
    const under = path + "/";
    for (const n of [...store.dirs])
        if (n === path || n.startsWith(under))
            store.dirs.delete(n);
    for (const n of [...store.files.keys()])
        if (n === path || n.startsWith(under)) {
            store.files.delete(n);
            store.mtimes.delete(n);
        }
}

export class FakeStore {
    constructor() {
        this.reset();
    }

    reset() {
        this.dirs = new Set(["/"]);
        this.files = new Map(); // path -> Uint8Array
        this.mtimes = new Map(); // path -> epoch ms; a directory has none
        this.clock = CLOCK_START;
        this.handles = [];      // index -> path, or null
        this.opfs = true;
        this.sync = true;
        this.persisted = false;
        this.quota = 10737418240;
        // The archive already parsed: parseZip is async and every reply here
        // is synchronous, so run.mjs reads it during its own setup and this
        // side only installs it. The parser still runs over the real bytes.
        this.entries = null;
        this.unpacks = 0;
        this.defer = false;
        this.held = [];         // tokens whose replies are being withheld
    }

    usage() {
        let n = 0;
        for (const data of this.files.values())
            n += data.length;
        return n;
    }

    // Every write moves the clock on, so `ls -t` has something to sort.
    stamp(path) {
        this.clock += CLOCK_STEP;
        this.mtimes.set(path, this.clock);
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
        const path = op === OP.INFO || op === OP.UNPACK ? "" : r.arg();
        if (path && notDir(store, path))
            return r.fail(E.NOTDIR);
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

        case OP.UNPACK: {
            if (!store.entries)
                return r.fail(E.NOTFOUND);
            store.unpacks++;
            for (const step of installOps(store.entries, r.arg())) {
                if (step.op === "remove")
                    removeTree(store, step.path);
                else if (step.op === "mkdir")
                    store.dirs.add(step.path);
                else {
                    store.files.set(step.path, step.bytes);
                    store.stamp(step.path);
                }
            }
            return r.ok(store.entries.length);
        }

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
                store.stamp(path);
            } else if (flags & O_TRUNC) {
                store.files.set(path, new Uint8Array(0));
                store.stamp(path);
            }
            let slot = store.handles.indexOf(null);
            if (slot < 0)
                slot = store.handles.push(null) - 1;
            store.handles[slot] = path;
            return r.ok(slot);
        }

        case OP.STAT: {
            r.set("status", 0);
            if (store.dirs.has(path))
                return r.write(packStat({ dir: true, size: 0, mtime: 0 }));
            const data = store.files.get(path);
            if (!data)
                return r.fail(E.NOTFOUND);
            const target = linkTarget(data);
            return r.write(packStat({
                dir: false,
                link: target !== null,
                size: target !== null ? target.length : data.length,
                mtime: store.mtimes.get(path) || 0,
            }));
        }

        case OP.LIST: {
            if (!store.dirs.has(path))
                return r.fail(store.files.has(path) ? E.NOTDIR : E.NOTFOUND);
            const out = [];
            const under = path === "/" ? "/" : path + "/";
            for (const name of store.dirs)
                if (name !== "/" && dirname(name) === path)
                    out.push({ name: name.slice(under.length), dir: true, size: 0, mtime: 0 });
            for (const [name, data] of store.files)
                if (dirname(name) === path) {
                    const target = linkTarget(data);
                    out.push({
                        name: name.slice(under.length),
                        dir: false,
                        link: target !== null,
                        size: target !== null ? target.length : data.length,
                        mtime: store.mtimes.get(name) || 0,
                    });
                }
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
            if (store.files.delete(path)) {
                store.mtimes.delete(path);
                return r.ok();
            }
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
                store.mtimes.delete(n);
            }
            store.dirs.delete(path);
            return r.ok();
        }

        case OP.TOUCH:
            // NOTDIR on a directory, which is what OPFS's TypeMismatchError
            // becomes when getFileHandle is handed one.
            if (!store.files.has(path))
                return r.fail(store.dirs.has(path) ? E.NOTDIR : E.NOTFOUND);
            store.stamp(path);
            return r.ok();

        case OP.SYMLINK: {
            // The bytes the real store holds, so one linkTarget() serves both.
            if (store.dirs.has(path) || store.files.has(path))
                return r.fail(E.EXISTS);
            if (!store.dirs.has(dirname(path)))
                return r.fail(E.NOTFOUND);
            store.files.set(path, linkBytes(r.text()));
            store.stamp(path);
            return r.ok();
        }

        case OP.READLINK: {
            const data = store.files.get(path);
            if (!data)
                return r.fail(store.dirs.has(path) ? E.INVALID : E.NOTFOUND);
            const target = linkTarget(data);
            if (target === null)
                return r.fail(E.INVALID);
            r.set("status", 0);
            return r.write(new TextEncoder().encode(target));
        }

        case OP.RENAME: {
            // Files and links alone, as OPFS move() is, so the caller's copy
            // fallback is the path a directory takes here too. The mtime rides
            // along: not rewriting the bytes is the whole point of the fast path.
            const to = r.text();
            if (store.dirs.has(path) || store.dirs.has(to))
                return r.fail(E.UNSUPPORTED);
            if (!store.files.has(path))
                return r.fail(E.NOTFOUND);
            if (notDir(store, to))
                return r.fail(E.NOTDIR);
            if (!store.dirs.has(dirname(to)))
                return r.fail(E.NOTFOUND);
            store.files.set(to, store.files.get(path));
            store.mtimes.set(to, store.mtimes.get(path) || 0);
            store.files.delete(path);
            store.mtimes.delete(path);
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
                store.stamp(path);
                return len;
            }
            case SYNC.SIZE:
                return data.length;
            case SYNC.TRUNCATE: {
                const out = new Uint8Array(off);
                out.set(data.subarray(0, Math.min(off, data.length)));
                store.files.set(path, out);
                store.stamp(path);
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

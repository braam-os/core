// The JS half of the storage ABI (Concept.md §5.2). Two imports: `fs` starts an
// asynchronous operation and answers through wake(), and `fs_sync` is the
// sanctioned §2.2 exception — once a sync access handle exists, reading and
// writing a file really are synchronous.
//
// The FsRequest layout below mirrors src/fs/hostfs.h field for field. Change
// one and change the other.

export const OP = {
    INFO: 1,
    BUNDLE: 2,
    OPEN: 3,
    STAT: 4,
    LIST: 5,
    MKDIR: 6,
    REMOVE: 7,
};

export const SYNC = {
    READ: 1,
    WRITE: 2,
    SIZE: 3,
    TRUNCATE: 4,
    FLUSH: 5,
    CLOSE: 6,
};

// Open flags, from src/fs/fs.h. Only creation and truncation reach OPFS: a
// sync access handle is read-write regardless of what the opener asked for.
export const O_CREATE = 4, O_TRUNC = 8;

// src/kernel/result.h's Error, for the values this file reports.
export const E = {
    INVALID: 1, NOMEMORY: 2, NOTFOUND: 3, EXISTS: 4, NOTDIR: 5, ISDIR: 6,
    PERM: 7, IO: 8, UNSUPPORTED: 11, NOTEMPTY: 13,
};

// Field offsets in words, matching struct FsRequest.
const F = {
    op: 0, token: 1, pathPtr: 2, pathLen: 3, flags: 4,
    bufPtr: 5, bufCap: 6, status: 7, resultLo: 8, resultHi: 9, bufLen: 10,
};

export class Request {
    constructor(mem, addr) {
        this.mem = mem;
        this.at = addr >>> 2;
    }

    get(field) {
        return this.mem.u32()[this.at + F[field]];
    }

    set(field, value) {
        this.mem.u32()[this.at + F[field]] = value >>> 0;
    }

    path() {
        return this.mem.str(this.get("pathPtr"), this.get("pathLen"));
    }

    ok(lo = 0, hi = 0) {
        this.set("status", 0);
        this.set("resultLo", lo);
        this.set("resultHi", hi);
    }

    fail(code) {
        this.mem.u32()[this.at + F.status] = (-code) >>> 0;
    }

    // Copies a reply into the buffer the kernel supplied, or reports how much
    // room it would have needed (src/fs/hostfs.h's two-phase reply).
    write(bytes) {
        const cap = this.get("bufCap");
        if (bytes.length > cap) {
            this.set("bufLen", 0);
            this.set("resultLo", bytes.length);
            this.set("status", 0);
            return;
        }
        this.mem.view().set(bytes, this.get("bufPtr"));
        this.set("bufLen", bytes.length);
        this.set("status", 0);
    }
}

// The reply the kernel decodes in fs_decode_entries: per entry, four u32s and
// then the name, padded up to the next word.
export function packEntries(entries) {
    let size = 0;
    const names = entries.map((e) => new TextEncoder().encode(e.name));
    for (const n of names)
        size += 16 + ((n.length + 3) & ~3);

    const out = new Uint8Array(size);
    const words = new Uint32Array(out.buffer);
    let at = 0;
    entries.forEach((e, i) => {
        const n = names[i];
        words[at >> 2] = e.dir ? 1 : 0;
        words[(at >> 2) + 1] = e.size >>> 0;
        words[(at >> 2) + 2] = Math.floor(e.size / 4294967296) >>> 0;
        words[(at >> 2) + 3] = n.length;
        out.set(n, at + 16);
        at += 16 + ((n.length + 3) & ~3);
    });
    return out;
}

export function packInfo(info) {
    const out = new Uint8Array(32);
    const words = new Uint32Array(out.buffer);
    words[0] = info.quota >>> 0;
    words[1] = Math.floor(info.quota / 4294967296) >>> 0;
    words[2] = info.usage >>> 0;
    words[3] = Math.floor(info.usage / 4294967296) >>> 0;
    words[4] = info.opfs ? 1 : 0;
    words[5] = info.sync ? 1 : 0;
    words[6] = info.fsaccess ? 1 : 0;
    words[7] = info.persisted ? 1 : 0;
    return out;
}

// Splits "/a/b" into ["a", "b"]. Paths arrive already normalised.
function parts(path) {
    return path.split("/").filter((s) => s.length > 0);
}

// The OPFS backend. Every method may reject; the caller turns that into a
// status, so a browser quirk becomes an error value rather than a dead kernel.
export class OpfsStore {
    constructor(root, bundle, persisted) {
        this.root = root;               // FileSystemDirectoryHandle, or null
        this.bundle = bundle;           // Uint8Array, or null
        this.persisted = !!persisted;
        this.handles = [];              // index -> FileSystemSyncAccessHandle
        this.sync = typeof FileSystemFileHandle !== "undefined" &&
            !!FileSystemFileHandle.prototype.createSyncAccessHandle;
    }

    // A plain array rather than the externref table of Concept.md §3.7: the
    // table arrives with M6, and a handle is already just an index here.
    slot(handle) {
        return this.handles[handle] || null;
    }

    async dir(path, create) {
        let at = this.root;
        for (const name of parts(path))
            at = await at.getDirectoryHandle(name, { create });
        return at;
    }

    async info() {
        const est = (navigator.storage && navigator.storage.estimate)
            ? await navigator.storage.estimate()
            : {};
        return {
            quota: est.quota || 0,
            usage: est.usage || 0,
            opfs: !!this.root,
            sync: !!this.root && this.sync,
            fsaccess: typeof self.showDirectoryPicker === "function",
            persisted: this.persisted,
        };
    }

    async open(path, flags) {
        const at = path.lastIndexOf("/");
        const dir = await this.dir(path.slice(0, at), false);
        const create = (flags & O_CREATE) !== 0;
        const file = await dir.getFileHandle(path.slice(at + 1), { create });
        const handle = await file.createSyncAccessHandle();
        if (flags & O_TRUNC)
            handle.truncate(0);

        let slot = this.handles.indexOf(null);
        if (slot < 0)
            slot = this.handles.push(null) - 1;
        this.handles[slot] = handle;
        return slot;
    }

    async stat(path) {
        if (path === "/")
            return { dir: true, size: 0 };
        const at = path.lastIndexOf("/");
        const dir = await this.dir(path.slice(0, at), false);
        const name = path.slice(at + 1);
        try {
            const file = await dir.getFileHandle(name);
            return { dir: false, size: (await file.getFile()).size };
        } catch {
            await dir.getDirectoryHandle(name);
            return { dir: true, size: 0 };
        }
    }

    async list(path) {
        const dir = await this.dir(path, false);
        const out = [];
        for await (const [name, handle] of dir.entries()) {
            const isDir = handle.kind === "directory";
            out.push({
                name,
                dir: isDir,
                size: isDir ? 0 : (await handle.getFile()).size,
            });
        }
        return out;
    }

    async mkdir(path) {
        const at = path.lastIndexOf("/");
        const dir = await this.dir(path.slice(0, at), false);
        const name = path.slice(at + 1);
        // getDirectoryHandle with create is happy to succeed twice, and mkdir
        // is not: an existing name has to be an error the shell can report.
        try {
            await dir.getDirectoryHandle(name);
            throw { braam: E.EXISTS };
        } catch (e) {
            if (e && e.braam)
                throw e;
        }
        await dir.getDirectoryHandle(name, { create: true });
    }

    async remove(path, recursive) {
        const at = path.lastIndexOf("/");
        const dir = await this.dir(path.slice(0, at), false);
        await dir.removeEntry(path.slice(at + 1), { recursive });
    }
}

// Maps a rejection onto one of src/kernel/result.h's Error values.
function statusOf(e) {
    if (e && e.braam)
        return e.braam;
    const name = e && e.name;
    if (name === "NotFoundError")
        return E.NOTFOUND;
    if (name === "TypeMismatchError")
        return E.NOTDIR;
    if (name === "InvalidModificationError")
        return E.NOTEMPTY;
    if (name === "NoModificationAllowedError")
        return E.PERM;
    if (name === "QuotaExceededError")
        return E.IO;
    return E.IO;
}

// Builds the two imports. `reply` delivers a finished request to the kernel;
// it must never run inside the import call itself, or wake() would re-enter a
// tick that is still on the stack. Every path below goes through a promise,
// so it cannot.
export function makeFsImports(mem, store, reply) {
    async function perform(r, op) {
        switch (op) {
        case OP.INFO:
            r.write(packInfo(await store.info()));
            return;
        case OP.BUNDLE:
            if (!store.bundle) {
                r.set("resultLo", 0);
                r.set("bufLen", 0);
                r.set("status", 0);
                return;
            }
            r.write(store.bundle);
            return;
        case OP.OPEN:
            r.ok(await store.open(r.path(), r.get("flags")));
            return;
        case OP.STAT: {
            const s = await store.stat(r.path());
            r.set("flags", s.dir ? 1 : 0);
            r.ok(s.size >>> 0, Math.floor(s.size / 4294967296) >>> 0);
            return;
        }
        case OP.LIST:
            r.set("status", 0);
            r.write(packEntries(await store.list(r.path())));
            return;
        case OP.MKDIR:
            await store.mkdir(r.path());
            r.ok();
            return;
        case OP.REMOVE:
            await store.remove(r.path(), (r.get("flags") & 1) !== 0);
            r.ok();
            return;
        default:
            r.fail(E.UNSUPPORTED);
        }
    }

    return {
        fs(op, token, req) {
            const r = new Request(mem, req);
            if (!store.root && op !== OP.INFO && op !== OP.BUNDLE) {
                Promise.resolve().then(() => {
                    r.fail(E.UNSUPPORTED);
                    reply(token);
                });
                return;
            }
            perform(r, op)
                .catch((e) => r.fail(statusOf(e)))
                .then(() => reply(token));
        },

        // Synchronous throughout, which is the point: a sync access handle
        // needs no promise, so no wake token is involved (Concept.md §5.2).
        fs_sync(op, handle, ptr, len, off) {
            const h = store.slot(handle);
            if (!h)
                return -E.INVALID;
            try {
                switch (op) {
                case SYNC.READ:
                    return h.read(mem.view().subarray(ptr, ptr + len), { at: off });
                case SYNC.WRITE:
                    return h.write(mem.view().subarray(ptr, ptr + len), { at: off });
                case SYNC.SIZE:
                    return h.getSize();
                case SYNC.TRUNCATE:
                    h.truncate(off);
                    return 0;
                case SYNC.FLUSH:
                    h.flush();
                    return 0;
                case SYNC.CLOSE:
                    // Flushing here rather than on every write: a sync handle
                    // buffers, and closing is the only point we know is safe.
                    try {
                        h.flush();
                    } catch { /* a read-only handle has nothing to flush */ }
                    h.close();
                    store.handles[handle] = null;
                    return 0;
                default:
                    return -E.UNSUPPORTED;
                }
            } catch (e) {
                return -statusOf(e);
            }
        },
    };
}

// Probes for OPFS and loads the boot bundle. Returns a store either way: with
// no OPFS the kernel falls back to MemFs and says so (Concept.md §5.2).
export async function openStore(bundleUrl, persisted) {
    let root = null;
    try {
        if (navigator.storage && navigator.storage.getDirectory)
            root = await navigator.storage.getDirectory();
    } catch {
        root = null;
    }

    let bundle = null;
    try {
        const res = await fetch(bundleUrl);
        if (res.ok)
            bundle = new Uint8Array(await res.arrayBuffer());
    } catch {
        bundle = null;
    }

    return new OpfsStore(root, bundle, persisted);
}

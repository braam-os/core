// The JS half of the storage ABI (Concept.md §5.2). Two imports: `fs` starts an
// asynchronous operation and answers through wake(), and `fs_sync` is the
// sanctioned §2.2 exception — once a sync access handle exists, reading and
// writing a file really are synchronous.
//
// The request record itself is in abi.js, shared with host services.

import { E, Request, statusOf } from "./abi.js";

export { E, Request, statusOf };

export const OP = {
    INFO: 1,
    UNPACK: 2,
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

// Where the unpacked archive's version is left, for boot to compare against
// the kernel's own (src/user/boot.cpp).
export const VERSION_PATH = "/version";

const u16 = (b, at) => b[at] | (b[at + 1] << 8);
const u32 = (b, at) => (u16(b, at) | (u16(b, at + 2) << 16)) >>> 0;

// A name out of an archive is not a path until it has been looked at: an
// absolute one, a drive letter or a `..` component would be written outside
// the root. Same-origin or not, this is the one zip bug worth checking by hand.
function safeName(name) {
    if (!name || name.startsWith("/") || name.includes("\\") || /^[A-Za-z]:/.test(name))
        return false;
    return !name.split("/").some((c) => c === "." || c === "..");
}

async function inflate(bytes) {
    const s = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
    return new Uint8Array(await new Response(s).arrayBuffer());
}

// rootfs.zip -> [{name, bytes}]. An ordinary deflated zip, read through the
// central directory: the end record points at it, and each of its entries
// points at a local header whose own name and extra lengths say where the data
// begins. Taking the central directory's offset for the data is the classic
// way to get this wrong.
//
// Async, because there is no synchronous inflate; installOps below is not, so
// a caller that cannot await inside a call can parse ahead of time.
export async function parseZip(bytes) {
    const bad = () => { throw { braam: E.INVALID }; };

    let eocd = -1;
    for (let at = bytes.length - 22; at >= 0 && at >= bytes.length - 22 - 0xffff; at--) {
        if (u32(bytes, at) === 0x06054b50) {
            eocd = at;
            break;
        }
    }
    if (eocd < 0)
        bad();

    const count = u16(bytes, eocd + 10);
    let at = u32(bytes, eocd + 16);
    // Zip64 announces itself by saturating these; nothing we pack comes near
    // either limit, so refusing is better than reading half an archive.
    if (count === 0xffff || at === 0xffffffff)
        throw { braam: E.UNSUPPORTED };

    const out = [];
    for (let i = 0; i < count; i++) {
        if (at + 46 > bytes.length || u32(bytes, at) !== 0x02014b50)
            bad();

        const flags = u16(bytes, at + 8);
        const method = u16(bytes, at + 10);
        const packed = u32(bytes, at + 20);
        const nameLen = u16(bytes, at + 28);
        const local = u32(bytes, at + 42);
        const name = new TextDecoder().decode(bytes.subarray(at + 46, at + 46 + nameLen));
        at += 46 + nameLen + u16(bytes, at + 30) + u16(bytes, at + 32);

        if (flags & 1)
            throw { braam: E.UNSUPPORTED }; // encrypted
        if (name.endsWith("/"))
            continue; // a directory entry; the paths below imply it anyway
        if (!safeName(name))
            bad();

        if (local + 30 > bytes.length || u32(bytes, local) !== 0x04034b50)
            bad();
        const from = local + 30 + u16(bytes, local + 26) + u16(bytes, local + 28);
        if (from + packed > bytes.length)
            bad();

        const data = bytes.subarray(from, from + packed);
        if (method === 0)
            out.push({ name, bytes: data.slice() });
        else if (method === 8)
            out.push({ name, bytes: await inflate(data) });
        else
            throw { braam: E.UNSUPPORTED };
    }
    return out;
}

// What installing an archive *is*, as operations for the caller to perform:
// OPFS awaits each one and the test fake does them synchronously, and neither
// has its own idea of what the archive means.
//
// The top-level directories the archive carries are removed first, so a name
// that is gone from a new release is gone from the store. Whatever it does not
// carry — /home, /tmp, /import — is never named and so never touched.
export function *installOps(entries, version) {
    const tops = new Set(entries.map((e) => e.name.split("/")[0]));
    for (const top of [...tops].sort())
        yield { op: "remove", path: "/" + top };

    const made = new Set();
    for (const e of entries) {
        const dirs = e.name.split("/").slice(0, -1);
        for (let i = 1; i <= dirs.length; i++) {
            const path = "/" + dirs.slice(0, i).join("/");
            if (!made.has(path)) {
                made.add(path);
                yield { op: "mkdir", path };
            }
        }
        yield { op: "write", path: "/" + e.name, bytes: e.bytes };
    }

    // Last, so an interrupted unpack leaves a stamp that does not match and is
    // therefore done again rather than believed.
    yield { op: "write", path: VERSION_PATH, bytes: new TextEncoder().encode(version) };
}

// The OPFS backend. Every method may reject; the caller turns that into a
// status, so a browser quirk becomes an error value rather than a dead kernel.
export class OpfsStore {
    constructor(root, rootfsUrl, persisted) {
        this.root = root;               // FileSystemDirectoryHandle, or null
        this.rootfsUrl = rootfsUrl;     // fetched only to unpack
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

    // A whole file at once, and a handle of its own: the unpack runs at boot
    // with nothing else open, so the exclusive lock is uncontended and the
    // handle never enters the table descriptors are drawn from.
    async put(path, bytes) {
        const at = path.lastIndexOf("/");
        const dir = await this.dir(path.slice(0, at), true);
        const file = await dir.getFileHandle(path.slice(at + 1), { create: true });
        const h = await file.createSyncAccessHandle();
        try {
            h.truncate(0);
            h.write(bytes, { at: 0 });
            h.flush();
        } finally {
            h.close();
        }
    }

    // Fetched here rather than at boot, so a system whose stored image is
    // current never asks for the archive at all.
    async unpack(version) {
        const res = await fetch(this.rootfsUrl);
        if (!res.ok)
            throw { braam: E.NOTFOUND };
        const entries = await parseZip(new Uint8Array(await res.arrayBuffer()));

        for (const step of installOps(entries, version)) {
            if (step.op === "remove")
                await this.remove(step.path, true).catch(() => {});
            else if (step.op === "mkdir")
                await this.dir(step.path, true);
            else
                await this.put(step.path, step.bytes);
        }
        return entries.length;
    }
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
        case OP.UNPACK:
            // The argument is the version to stamp, not a path.
            r.ok(await store.unpack(r.arg()));
            return;
        case OP.OPEN:
            r.ok(await store.open(r.arg(), r.get("flags")));
            return;
        case OP.STAT: {
            const s = await store.stat(r.arg());
            r.set("flags", s.dir ? 1 : 0);
            r.ok(s.size >>> 0, Math.floor(s.size / 4294967296) >>> 0);
            return;
        }
        case OP.LIST:
            r.set("status", 0);
            r.write(packEntries(await store.list(r.arg())));
            return;
        case OP.MKDIR:
            await store.mkdir(r.arg());
            r.ok();
            return;
        case OP.REMOVE:
            await store.remove(r.arg(), (r.get("flags") & 1) !== 0);
            r.ok();
            return;
        default:
            r.fail(E.UNSUPPORTED);
        }
    }

    return {
        fs(op, token, req) {
            const r = new Request(mem, req);
            if (!store.root && op !== OP.INFO) {
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

// Probes for OPFS. Returns a store either way: a root of null is what the
// kernel reads as "no OPFS", which it now reports as fatal (Concept.md §5.2).
// The archive is not fetched here — only an unpack needs it.
export async function openStore(rootfsUrl, persisted) {
    let root = null;
    try {
        if (navigator.storage && navigator.storage.getDirectory)
            root = await navigator.storage.getDirectory();
    } catch {
        root = null;
    }

    return new OpfsStore(root, rootfsUrl, persisted);
}

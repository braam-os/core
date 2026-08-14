// The host's half of the process tiers (Concept.md §4), and the process-side
// half of the tier-3 protocol beside it, so the two cannot drift.
//
// An isolated process is a second WebAssembly.Instance with a memory the kernel
// sized, an import closure bound to its pid, and an externref table of its own
// that it gets for free by being a separate module. Tier 2 puts that instance
// in this worker; tier 3 puts it in a worker of its own, where terminate() is a
// kill that does not need the process's cooperation.
//
// The rule everything here follows: **a process never runs while the kernel is
// on the stack.** Its exports reach the kernel through `sys`, and re-entering
// the kernel in the middle of a tick would run it on a heap it is halfway
// through changing. So a tier-2 step is queued and drained later, which is what
// `schedule` is for; a tier-3 step is a message, which is deferred by nature.
// Compiling and instantiating execute no wasm, so they need no such care.

import { E } from "./abi.js";
import { Memory } from "./host.js";

// src/kernel/sysabi.h's ProcStep and Tier.
export const STEP = { EXITED: 0, SUSPENDED: 1, TRAPPED: 2 };
export const TIER = { INSTANCE: 2, WORKER: 3 };

// src/kernel/sysabi.h's Sys. The kernel answers the whole table at tier 2; at
// tier 3 the synchronous half is answered in the process's own worker, because
// a worker boundary has no synchronous direction (Concept.md §4.3).
const SYS = { EXIT: 1, GETPID: 2, NOW: 3, STAGE: 4 };

// src/kernel/sysabi.h's proc_pack: the page counts and the tier in one word.
const initialOf = (flags) => flags & 0xffff;
const maxOf = (flags) => (flags >>> 16) & 0xfff;
const tierOf = (flags) => flags >>> 28;

// How many workers stay hired with no process in them. The pool saves the cost
// of starting one, not memory: a process's pages go when its instance does.
const MAX_IDLE = 2;

// ------------------------------------------------------- the process's half

// One instance, and the two calls it can make. `ops` is where the tiers differ
// and the only place they do: at tier 2 it reaches straight into the kernel, at
// tier 3 it answers what it can and records the rest for the step's reply.
export function serveProc(ops) {
    const mem = new Memory();
    let instance = null;
    let started = false;

    return {
        mem,

        // Also the reset a pooled worker gets: the previous instance goes, and
        // the whole of its memory with it.
        bind(module, initial, maximum) {
            instance = null;
            started = false;
            const memory = new WebAssembly.Memory({ initial, maximum });
            mem.bind(memory);
            instance = new WebAssembly.Instance(module, {
                env: { memory },
                kernel: {
                    sys(op, a0, a1, a2) {
                        return ops.sys(op >>> 0, a0 >>> 0, a1 >>> 0, a2 >>> 0);
                    },

                    // The payload lives in the process's memory and the kernel
                    // cannot reach it, so the copy happens on the way out
                    // (Appendix B). The token goes with it: a process may have
                    // several calls outstanding, and the kernel says which one
                    // it is answering when it steps.
                    sys_async(op, tok, ptr, len) {
                        ops.sysAsync(op >>> 0, tok >>> 0, ptr >>> 0, len >>> 0, mem);
                    },
                },
            });
        },

        // One _start or _resume. `payload` is argv the first time and a syscall
        // reply afterwards, and `token` names the call it answers; it goes into
        // the process's own heap through its own allocator, and _resume frees
        // it.
        step(token, payload) {
            if (!instance)
                return { result: STEP.TRAPPED };

            let ptr = 0;
            if (payload.length) {
                ptr = instance.exports._alloc(payload.length) >>> 0;
                if (!ptr)
                    return { fail: E.NOMEMORY };
                mem.view().set(payload, ptr);
            }

            try {
                const out = started
                    ? instance.exports._resume(token >>> 0, ptr, payload.length)
                    : instance.exports._start(ptr, payload.length);
                started = true;
                return { result: out === 0 ? STEP.EXITED : STEP.SUSPENDED };
            } catch {
                // A trap is how a process reports a fatal error, and there is
                // nothing left to resume: the instance goes.
                instance = null;
                return { result: STEP.TRAPPED };
            }
        },
    };
}

// The tier-3 half of `ops`, running in the process's own worker. Nothing here
// reaches the kernel: `getpid` is the pid the host bound in, `now` is a clock
// the step message carried, `exit` and the asynchronous call ride back on the
// step's reply, and `stage` is refused — it is the host's syscall rather than a
// program's, and a hostile binary may still call it.
export function workerOps(pid, clock) {
    let base = 0;
    let at = 0;
    let exit;
    let calls = [];

    return {
        sys(op, a0) {
            switch (op) {
            case SYS.EXIT:
                exit = a0 | 0;
                return 0;
            case SYS.GETPID:
                return pid;
            case SYS.NOW:
                return (base + (clock() - at)) >>> 0;
            case SYS.STAGE:
                return 0;
            default:
                return -E.UNSUPPORTED;
            }
        },

        sysAsync(op, token, ptr, len, mem) {
            // slice, not subarray: the buffer is transferred, and a later
            // memory.grow would detach a view onto the instance's own memory
            // (Concept.md §8.4).
            //
            // A list, because one step can park more than one task: resuming
            // the root may start a second and leave both waiting.
            calls.push({ op, token, len, payload: mem.view().slice(ptr, ptr + len).buffer });
        },

        begin(now) {
            base = now;
            at = clock();
            exit = undefined;
            calls = [];
        },

        end() {
            return { exit, calls };
        },
    };
}

// ---------------------------------------------------------- the host's half

// `schedule(drain)` must call drain() once the kernel is off the stack; a
// caller that passes nothing drains by hand, which is what the tests do.
// `makeLink()` makes a worker for a tier-3 process, or is absent, in which case
// a binary asking for tier 3 runs at tier 2 (Concept.md §4).
export function makeProc(mem, kernel, schedule, makeLink, clock = () => 0) {
    const modules = new Map(); // path -> Module; §4.4's compile cache
    const procs = new Map();   // pid -> the kernel's half of one process
    const queue = [];          // tier-2 steps, waiting for the kernel to unwind
    const idle = [];           // workers with no process in them
    let workers = true;        // until one refuses to be made

    // The capability boundary in a dozen lines: these two closures are all a
    // tier-2 process can call, and the pid is written into them here rather
    // than passed by the caller. Process 7 holds no function that says 3.
    function localOps(pid) {
        return {
            sys(op, a0, a1, a2) {
                return kernel().sys(pid, op, a0, a1, a2);
            },

            // The kernel is asked for the destination first; a zero means it
            // could not make room, and the length goes over regardless so that
            // the reply can say so.
            sysAsync(op, token, ptr, len, from) {
                const dst = kernel().sys(pid, SYS.STAGE, len, 0, 0) >>> 0;
                if (dst && len)
                    mem.view().set(from.view().subarray(ptr, ptr + len), dst);
                kernel().sys_async(pid, op, token, len);
            },
        };
    }

    // A link is one worker. It is hired without a process in it, which is also
    // the capability probe: where nested workers do not exist the constructor
    // throws here, at boot, rather than under the first `exec`.
    function hire() {
        if (!workers || !makeLink)
            return null;
        try {
            const link = makeLink();
            link.onmessage = ({ data }) => deliver(link, data);
            link.onerror = () => broke(link);
            link.pid = 0;
            return link;
        } catch {
            workers = false;
            return null;
        }
    }

    function take() {
        return idle.pop() || hire();
    }

    function pool(link) {
        link.pid = 0;
        if (idle.length < MAX_IDLE)
            idle.push(link);
        else
            link.terminate();
    }

    // A worker that failed to load or threw where nothing could catch it. Its
    // process is a crashed one, and the link goes rather than being pooled —
    // hence the `p.link = null`, which is what stops kill() from handing a
    // dead worker back to the next process.
    function broke(link) {
        const at = idle.indexOf(link);
        if (at >= 0)
            idle.splice(at, 1);
        const p = procs.get(link.pid);
        if (p && p.link === link) {
            p.link = null;
            finish(p, { result: STEP.TRAPPED });
        }
        link.pid = 0;
        link.terminate();
        if (!idle.length && !procs.size)
            workers = false; // it never worked; stop trying
    }

    function spawn(r) {
        const pid = r.get("aux");
        const path = r.arg();
        const flags = r.get("flags");
        const initial = initialOf(flags);
        const maximum = maxOf(flags);

        // Compiled here whatever the tier: the cache is the host's (§4.4), a
        // Module is structured-cloneable, and a malformed binary is caught
        // where `exec` can still say so.
        let module = modules.get(path);
        if (!module) {
            module = new WebAssembly.Module(r.bytes());
            modules.set(path, module);
        }

        const link = tierOf(flags) === TIER.WORKER ? take() : null;
        const p = { pid, link, server: null, pending: null, done: false };

        // The cap is the kernel's word and not the binary's: the module
        // declares no maximum of its own, so memory.grow stops at the number
        // that came over — which is an rlimit without cgroups (§4.1).
        if (link) {
            link.pid = pid;
            link.postMessage({ k: "bind", pid, module, initial, maximum });
        } else {
            p.server = serveProc(localOps(pid));
            p.server.bind(module, initial, maximum);
        }
        procs.set(pid, p);
        r.ok();
    }

    // `done()` answers the request. It must never reach wake() on this stack —
    // kill() is called from inside a host import — so both callers of makeProc
    // defer it.
    function step(r, done) {
        const pid = r.get("aux");
        const p = procs.get(pid);

        if (p && p.link && !p.done) {
            p.pending = { r, done };
            const payload = r.bytes();
            p.link.postMessage(
                { k: "step", now: clock(), token: r.get("flags"), payload: payload.buffer },
                [payload.buffer]);
            return;
        }

        queue.push({ pid, r, done });
        if (schedule)
            schedule(drain);
    }

    // A step whose result was the process's last: the instance is gone, and at
    // tier 3 the worker that held it is clean enough to hire out again.
    function retire(p) {
        p.done = true;
        p.server = null;
    }

    // The tier-3 reply, which is the tier-2 closure's work done a message
    // later: the exit status the process reported, then the asynchronous call
    // it parked on, then the answer to the step itself.
    function finish(p, m) {
        const pending = p.pending;
        p.pending = null;

        if (m.fail !== undefined) {
            retire(p);
            if (pending) {
                pending.r.fail(m.fail);
                pending.done();
            }
            return;
        }

        if (m.exit !== undefined)
            kernel().sys(p.pid, SYS.EXIT, m.exit, 0, 0);

        // One step can park more than one task, so the calls come back as a
        // list. The token is the process's own and rides with each: the kernel
        // will name it again when it answers.
        for (const call of m.calls || []) {
            const dst = kernel().sys(p.pid, SYS.STAGE, call.len, 0, 0) >>> 0;
            if (dst && call.len)
                mem.view().set(new Uint8Array(call.payload), dst);
            kernel().sys_async(p.pid, call.op, call.token, call.len);
        }

        if (m.result !== STEP.SUSPENDED)
            retire(p);
        if (pending) {
            pending.r.ok(m.result);
            pending.done();
        }
    }

    function deliver(link, m) {
        if (m.k !== "step")
            return; // "ready" says the worker loaded, and nothing more
        const p = procs.get(link.pid);
        if (p && p.link === link)
            finish(p, m);
    }

    function drain() {
        while (queue.length) {
            const { pid, r, done } = queue.shift();
            const p = procs.get(pid);
            if (!p || !p.server || p.done) {
                r.fail(E.NOTFOUND); // killed while this step was in flight
            } else {
                const out = p.server.step(r.get("flags"), r.bytes());
                if (out.fail !== undefined) {
                    retire(p);
                    r.fail(out.fail);
                } else {
                    if (out.result !== STEP.SUSPENDED)
                        retire(p);
                    r.ok(out.result);
                }
            }
            done();
        }
    }

    // Told, not asked, and immediate. At tier 2 dropping the entry is all it
    // takes; at tier 3 it is terminate(), which is the whole point of the tier
    // — a process in a loop between syscalls has no other way out. A worker
    // that had already finished its process is pooled instead, since `exec`
    // kills every process it spawned, including the ones that exited.
    function kill(pid) {
        const p = procs.get(pid);
        if (!p)
            return;
        procs.delete(pid);
        if (!p.link)
            return;

        if (p.done && !p.pending) {
            pool(p.link);
            return;
        }

        p.link.pid = 0;
        p.link.terminate();

        // The request the terminated worker will never answer. Nothing else
        // frees it: an abandoned record is reaped by wake() on its token, and
        // that only happens if somebody answers.
        if (p.pending) {
            const { r, done } = p.pending;
            p.pending = null;
            r.fail(E.NOTFOUND);
            done();
        }
    }

    // Letting go of the whole tier, for a host that is disposing of the kernel.
    function shutdown() {
        for (const link of idle)
            link.terminate();
        idle.length = 0;
        for (const p of procs.values())
            if (p.link)
                p.link.terminate();
        procs.clear();
    }

    function live() {
        return procs.size;
    }

    function pooled() {
        return idle.length;
    }

    // Steps this host still owes a tier-2 process, for a driver that has to
    // know whether draining by hand would do anything.
    function pending() {
        return queue.length;
    }

    // One worker hired before anything needs it: the first `exec` of a tier-3
    // binary then costs an instantiation rather than a worker start.
    const first = hire();
    if (first)
        idle.push(first);

    return { spawn, step, drain, kill, shutdown, live, pooled, pending };
}

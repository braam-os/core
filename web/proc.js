// The host's half of the process tier (Concept.md §4). An isolated process is
// a second WebAssembly.Instance in this worker, with a memory the kernel sized,
// an import closure bound to its pid, and an externref table of its own that it
// gets for free by being a separate module.
//
// The rule everything here follows: **a process never runs while the kernel is
// on the stack.** Its exports call straight back into the kernel through `sys`,
// and re-entering the kernel in the middle of a tick would run it on a heap it
// is halfway through changing. So a step is queued and drained later, which is
// what `schedule` is for. Compiling and instantiating execute no wasm, so they
// need no such care.

import { E } from "./abi.js";
import { Memory } from "./host.js";

// src/kernel/sysabi.h's ProcStep.
export const STEP = { EXITED: 0, SUSPENDED: 1, TRAPPED: 2 };

// The one syscall the host issues on its own behalf: it asks the kernel for
// room before copying a payload across (src/kernel/sysabi.h's Sys::Stage).
const SYS_STAGE = 4;

// `schedule(drain)` must call drain() once the kernel is off the stack; a
// caller that passes nothing drains by hand, which is what the tests do.
export function makeProc(mem, kernel, schedule) {
    const modules = new Map(); // path -> Module; §4.4's compile cache
    const procs = new Map();   // pid -> the instance and its state
    const queue = [];

    // The capability boundary in a dozen lines: these two closures are all a
    // process can call, and the pid is written into them here rather than
    // passed by the caller. Process 7 holds no function that says 3.
    function attach(pid, memory) {
        const p = { memory, mem: new Memory(), started: false, token: 0 };
        p.mem.bind(memory);
        p.imports = {
            env: { memory },
            kernel: {
                sys(op, a0, a1, a2) {
                    return kernel().sys(pid, op >>> 0, a0 >>> 0, a1 >>> 0, a2 >>> 0);
                },

                // The payload lives in the process's memory and the kernel
                // cannot reach it, so the copy happens here (Appendix B). The
                // kernel is asked for the destination first; a zero means it
                // could not make room, and the length goes over regardless so
                // that the reply can say so.
                sys_async(op, token, ptr, len) {
                    p.token = token >>> 0;
                    const n = len >>> 0;
                    const from = ptr >>> 0;
                    const dst = kernel().sys(pid, SYS_STAGE, n, 0, 0) >>> 0;
                    if (dst && n)
                        mem.view().set(p.mem.view().subarray(from, from + n), dst);
                    kernel().sys_async(pid, op >>> 0, token >>> 0, n);
                },
            },
        };
        return p;
    }

    function spawn(r) {
        const pid = r.get("aux");
        const path = r.arg();
        const initial = r.get("flags") & 0xffff;
        const maximum = r.get("flags") >>> 16;

        let module = modules.get(path);
        if (!module) {
            module = new WebAssembly.Module(r.bytes());
            modules.set(path, module);
        }

        // The cap is the kernel's word and not the binary's: the module
        // declares no maximum of its own, so memory.grow stops here — which is
        // an rlimit without cgroups (Concept.md §4.1).
        const p = attach(pid, new WebAssembly.Memory({ initial, maximum }));
        p.instance = new WebAssembly.Instance(module, p.imports);
        procs.set(pid, p);
        r.ok();
    }

    function run(r) {
        const pid = r.get("aux");
        const p = procs.get(pid);
        if (!p) {
            r.fail(E.NOTFOUND); // killed while this step was in flight
            return;
        }

        // argv the first time, a syscall reply afterwards. It goes into the
        // process's own heap through its own allocator, and _resume frees it.
        const payload = r.bytes();
        let ptr = 0;
        if (payload.length) {
            ptr = p.instance.exports._alloc(payload.length) >>> 0;
            if (!ptr) {
                r.fail(E.NOMEMORY);
                return;
            }
            p.mem.view().set(payload, ptr);
        }

        let out;
        try {
            out = p.started
                ? p.instance.exports._resume(p.token, ptr, payload.length)
                : p.instance.exports._start(ptr, payload.length);
            p.started = true;
        } catch {
            // A trap is how a process reports a fatal error, and there is
            // nothing left to resume: the instance goes.
            procs.delete(pid);
            r.ok(STEP.TRAPPED);
            return;
        }
        r.ok(out === 0 ? STEP.EXITED : STEP.SUSPENDED);
    }

    function step(r) {
        return new Promise((resolve) => {
            queue.push({ r, resolve });
            if (schedule)
                schedule(drain);
        });
    }

    function drain() {
        while (queue.length) {
            const { r, resolve } = queue.shift();
            run(r);
            resolve();
        }
    }

    // Told, not asked, and immediate: dropping the entry is all it takes, and
    // the whole of the process's memory goes with it.
    function kill(pid) {
        procs.delete(pid);
    }

    function live() {
        return procs.size;
    }

    return { spawn, step, drain, kill, live };
}

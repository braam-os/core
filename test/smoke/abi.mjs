// The kernel's import and export surface, and every binary's.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { readFileSync } from "node:fs";
import { basename } from "node:path";

import { compiled, fail, kernel, names, resize } from "./harness.mjs";

export function check(binaries) {
    // The import and export surface is the ABI; drift is a bug, and an
    // unexpected import means a libc dependency crept in.
    const want_imports = ["host.fs", "host.fs_sync", "host.log", "host.now", "host.present",
                          "host.svc"];
    const want_exports = ["init", "key", "memory", "ref", "resize", "sys", "sys_async", "tick",
                          "wake"];
    const got_imports = names(WebAssembly.Module.imports(compiled()));
    const got_exports = names(WebAssembly.Module.exports(compiled()));

    if (got_imports.join() !== want_imports.join())
        fail(`imports are [${got_imports}], expected [${want_imports}]`);
    if (got_exports.join() !== want_exports.join())
        fail(`exports are [${got_exports}], expected [${want_exports}]`);

    // The process ABI is a surface of its own (Concept.md §4.3), and the same
    // rule applies to it: drift is a bug. Note what is *not* there — a process
    // imports nothing from the host, and `sys` has no pid argument, which is
    // the whole of "a process cannot issue a syscall on behalf of another".
    // Every program is one of these and there is nothing else to be: a worker
    // of its own, the shell included, and no flag in the binary that says
    // otherwise (Concept.md §4).

    for (const binary of binaries) {
        const bin = new WebAssembly.Module(readFileSync(binary));
        const want_bin_imports = ["env.memory", "kernel.sys", "kernel.sys_async"];
        const want_bin_exports = ["_alloc", "_free", "_resume", "_start"];
        const got_bin_imports = names(WebAssembly.Module.imports(bin));
        const got_bin_exports = names(WebAssembly.Module.exports(bin));

        // A subset, not the whole list: `true` never makes an asynchronous
        // syscall, so it does not import sys_async at all. What is asserted is
        // that nothing *else* is imported — a host import in a binary would
        // mean the process ABI had been gone around.
        for (const name of got_bin_imports)
            if (!want_bin_imports.includes(name))
                fail(`${basename(binary)} imports ${name}, which is not the process ABI`);
        if (!got_bin_imports.includes("env.memory"))
            fail(`${basename(binary)} does not import env.memory`);
        if (got_bin_exports.join() !== want_bin_exports.join())
            fail(`${basename(binary)} exports [${got_bin_exports}], expected ` +
                 `[${want_bin_exports}]`);

        // The memory is imported, so its cap is the kernel's to set: the
        // module declares no maximum of its own to override it.
        const meta = WebAssembly.Module.customSections(bin, "braam");
        if (meta.length !== 1)
            fail(`${basename(binary)} carries ${meta.length} braam sections, expected 1`);
        const m = new Uint32Array(meta[0]);
        if (m[0] !== 0x6d617262 || m[1] !== 16)
            fail(`${basename(binary)}'s metadata is ${m[0].toString(16)}/${m[1]}`);
        if (m[4] !== 256)
            fail(`${basename(binary)} asks for ${m[4]} pages, expected 256`);
    }

    // The counts run.mjs prints when the suite is through.
    return { imports: got_imports.length, exports: got_exports.length };
}

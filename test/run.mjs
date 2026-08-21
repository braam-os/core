// Headless driver for kernel.wasm and tests.wasm. Node stands in for the
// browser: instantiating a freestanding module needs nothing browser-specific,
// and test/fakefs.mjs stands in for OPFS.
//
// The smoke suite is the ordered list below and nothing else. Every case is one
// file in test/smoke/, taking its kernel, screen and shell from
// test/smoke/harness.mjs; doc/Testing.md has the rules they run by. The order
// is load-bearing — the suite is one cumulative session — so an entry that
// depends on an earlier one says so beside it.

import { fail, hasRootfs, init, kernel } from "./smoke/harness.mjs";

import * as abi from "./smoke/abi.mjs";
import * as boot from "./smoke/boot.mjs";
import * as cwd from "./smoke/cwd.mjs";
import * as entry from "./smoke/entry.mjs";
import * as flow from "./smoke/flow.mjs";
import * as fullscreen from "./smoke/fullscreen.mjs";
import * as glob from "./smoke/glob.mjs";
import * as help from "./smoke/help.mjs";
import * as interrupt from "./smoke/interrupt.mjs";
import * as jobs from "./smoke/jobs.mjs";
import * as language from "./smoke/language.mjs";
import * as lists from "./smoke/lists.mjs";
import * as ls from "./smoke/ls.mjs";
import * as net from "./smoke/net.mjs";
import * as persist from "./smoke/persist.mjs";
import * as pipe from "./smoke/pipe.mjs";
import * as pkgcli from "./smoke/pkgcli.mjs";
import * as pkgClean from "./smoke/pkg-clean.mjs";
import * as pkgCrash from "./smoke/pkg-crash.mjs";
import * as pkgInstall from "./smoke/pkg-install.mjs";
import * as pkgRemove from "./smoke/pkg-remove.mjs";
import * as pkgScripts from "./smoke/pkg-scripts.mjs";
import * as pkgUpdate from "./smoke/pkg-update.mjs";
import * as pkgUpgrade from "./smoke/pkg-upgrade.mjs";
import * as pkgVerify from "./smoke/pkg-verify.mjs";
import * as procfs from "./smoke/procfs.mjs";
import * as process_ from "./smoke/process.mjs";
import * as redirect from "./smoke/redirect.mjs";
import * as rename from "./smoke/rename.mjs";
import * as respawn from "./smoke/respawn.mjs";
import * as script from "./smoke/script.mjs";
import * as sh from "./smoke/sh.mjs";
import * as spawn from "./smoke/spawn.mjs";
import * as subst from "./smoke/subst.mjs";
import * as sysinfo from "./smoke/sysinfo.mjs";
import * as term from "./smoke/term.mjs";
import * as vars from "./smoke/vars.mjs";
import * as worker from "./smoke/worker.mjs";
import * as wrap from "./smoke/wrap.mjs";

// A third field marks a case that reads the archive and is skipped without one.
const ARCHIVE = true;

const CASES = [
    ["abi",        abi.check],
    ["help",       help.check,       ARCHIVE],
    ["boot",       boot.check],       // boots the kernel: every case below needs a prompt
    ["term",       term.check],
    ["pipe",       pipe.check],
    ["vars",       vars.check],
    ["lists",      lists.check],
    ["flow",       flow.check],
    ["cwd",        cwd.check],        // leaves the shell in /home
    ["ls",         ls.check],         // after cwd: the fixture tree it lists
    ["sysinfo",    sysinfo.check],
    ["pkgcli",     pkgcli.check],
    // The eight share test/smoke/pkgfix.mjs and one clock, and run in this
    // order because each leaves the /pkg the next one starts from.
    ["pkg-update",  pkgUpdate.check,  ARCHIVE],
    ["pkg-install", pkgInstall.check, ARCHIVE],
    ["pkg-remove",  pkgRemove.check,  ARCHIVE], // after install: it left /pkg broken on purpose
    ["pkg-upgrade", pkgUpgrade.check, ARCHIVE],
    ["pkg-verify",  pkgVerify.check,  ARCHIVE],
    ["pkg-clean",   pkgClean.check,   ARCHIVE], // after upgrade: the generations it collects
    ["pkg-scripts", pkgScripts.check, ARCHIVE],
    ["pkg-crash",   pkgCrash.check,   ARCHIVE], // last: puts the shipped /etc back, drops /pkg
    ["procfs",     procfs.check],
    ["interrupt",  interrupt.check],
    ["persist",    persist.check],    // reloads the kernel and resets the store
    ["net",        net.check],        // after persist: a working store again
    ["fullscreen", fullscreen.check],
    ["jobs",       jobs.check],
    ["process",    process_.check],
    ["worker",     worker.check],     // after process: the worker under one
    ["spawn",      spawn.check],
    ["sh",         sh.check],
    ["wrap",       wrap.check],
    ["respawn",    respawn.check],    // takes workers away; its blocks are 1 s apart
    ["glob",       glob.check],
    ["rename",     rename.check],
    ["subst",      subst.check],
    ["redirect",   redirect.check],
    ["script",     script.check],
    ["entry",      entry.check],
    ["language",   language.check],   // last: it exits the shell for good
];

function usage() {
    console.error("usage: run.mjs --kernel <wasm> [--upto=<case>] [<rootfs.zip> [<proc.wasm>...]]" +
                  " | --tests <wasm> [<rootfs.zip>] | --list");
    process.exit(2);
}

if (process.argv.includes("--list")) {
    for (const [name] of CASES)
        console.log(name);
    process.exit(0);
}

const argv = process.argv.slice(2).filter((a) => !a.startsWith("--upto="));
const upto = process.argv.find((a) => a.startsWith("--upto="))?.slice(7);
const [mode, file, rootfs] = argv;
const binaries = argv.slice(3);
if (!file || (mode !== "--kernel" && mode !== "--tests"))
    usage();

await init(file, rootfs, mode);

if (upto && !CASES.some(([name]) => name === upto))
    fail(`--upto=${upto} names no case; --list prints them`);

if (mode === "--kernel") {
    // Each case leaves the state the next one starts from, so --upto is a
    // prefix rather than a filter: there is no way to run one on its own.
    let surface = { imports: 0, exports: 0 };
    for (const [name, check, archive] of CASES) {
        if (archive && !hasRootfs)
            continue;
        surface = check(binaries) ?? surface;
        if (name === upto) {
            console.log(`smoke ok: stopped after ${name}`);
            process.exit(0);
        }
    }
    console.log(`smoke ok: ${surface.imports} imports, ${surface.exports} exports`);
} else {
    const failures = kernel().run_tests();
    if (failures !== 0)
        fail(`${failures} check(s) failed`);
}

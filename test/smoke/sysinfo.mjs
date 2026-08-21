// df, /proc/host and uname.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { fail, names, prompt, row, rows, screen, store, submit } from "./harness.mjs";

export function check() {
    let s = screen();
    // M5, second criterion, as amended: df reports the quota and the usage as
    // a BSD table, the durability having moved to the boot banner. The whole
    // line is matched, since a row wider than this grid's sixty columns wraps.
    const blocks = Math.floor(store.quota / 1024);
    s = submit("clear", 1180);
    s = submit("df", 1181);
    const df = rows(s);
    if (!df.includes("Filesystem  1K-blocks     Used    Avail Capacity  Mounted on"))
        fail(`df did not head the table: ${JSON.stringify(df)}`);
    const root = df.find((line) => line.endsWith("    /"));
    if (!root || !new RegExp(`^opfs +${blocks} +\\d+ +\\d+ +\\d+%    /$`).test(root))
        fail(`df did not report the root: ${JSON.stringify(df)}`);
    if (!df.some((line) => /^procfs +0 +0 +0 +-    \/proc$/.test(line)))
        fail(`df did not report /proc: ${JSON.stringify(df)}`);

    // -h, over the fake's ten gibibytes exactly — the same figure as above.
    s = submit("clear", 1181.1);
    s = submit("df -h", 1181.2);
    const dfh = rows(s);
    if (!dfh.includes("Filesystem       Size     Used    Avail Capacity  Mounted on"))
        fail(`df -h did not head the table: ${JSON.stringify(dfh)}`);
    if (!dfh.some((line) => /^opfs +10G +\d/.test(line)))
        fail(`df -h did not scale the quota: ${JSON.stringify(dfh)}`);

    // /proc/host is what the kernel knows about itself and what the host said
    // about the browser at boot, and `uname` reformats it — the arrangement
    // `mount` has over /proc/mounts. The host half comes from the fake's fixed
    // string, so the whole file is deterministic.
    s = submit("clear", 1182);
    s = submit("cat /proc/host", 1183);
    const host = rows(s);
    // A plain table, colons and all left to the boot banner: `uname` reads a
    // field out of this with next_field, so a name must not carry punctuation.
    if (host.some((line) => /^[a-z]+:/.test(line)))
        fail(`/proc/host punctuates its names: ${JSON.stringify(host)}`);
    for (const want of ["system   braam", "machine  wasm32", "browser  Fake 1", "agent    fake"])
        if (!host.includes(want))
            fail(`/proc/host is missing ${JSON.stringify(want)}: ${JSON.stringify(host)}`);
    if (!host.some((line) => /^release  \d+\.\d+\.\d+/.test(line)))
        fail(`/proc/host did not report the release: ${JSON.stringify(host)}`);
    if (!host.includes(`screen   ${s.cols}x${s.rows}`))
        fail(`/proc/host did not report the geometry: ${JSON.stringify(host)}`);

    // The default is the system name; -m is the one field neither the host nor
    // the version supplies.
    s = submit("clear", 1184.1);
    s = submit("uname", 1184.2);
    if (!rows(s).includes("braam"))
        fail(`uname printed ${JSON.stringify(rows(s))}, expected braam`);
    s = submit("uname -m", 1184.3);
    if (!rows(s).includes("wasm32"))
        fail(`uname -m printed ${JSON.stringify(rows(s))}, expected wasm32`);

    // -a is every field, and the blank line splitting the banner's half from
    // the rest is a marker rather than something to print.
    s = submit("clear", 1184.4);
    s = submit("uname -a", 1184.5);
    const all = rows(s).filter((line) => line && !line.includes("$"));
    if (!all.includes("system   braam") || !all.includes("agent    fake"))
        fail(`uname -a printed ${JSON.stringify(all)}`);
    s = submit("uname -z", 1184.6);
    if (!rows(s).some((line) => line.startsWith("usage: uname ")))
        fail(`uname -z printed ${JSON.stringify(rows(s))}, expected a usage line`);
    if (!rows(s).includes(prompt(2)))
        fail(`uname -z left ${row(s, s.cursor_y)}, expected ${prompt(2)}`);
}

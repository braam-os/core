// init, the first tick, the screen, the first unpack and the motd.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import {
    KEY, SCREEN_MAGIC, cell, descriptor, fail, hasRootfs, kernel, logged, presented, press, prompt,
    regrid, resize, row, rows, run, screen, store, submit, type,
} from "./harness.mjs";

export function check() {

    kernel().init(0);

    if (logged.length !== 1)
        fail(`expected one boot line, got ${logged.length}`);
    if (!logged[0].startsWith("braam "))
        fail(`unexpected boot line: ${logged[0]}`);

    // init spawns the shell and nothing else. The first tick mounts the
    // filesystem, draws the prompt and parks the shell on the keyboard, so
    // there is nothing left pending.
    if (run(0) !== -1)
        fail("the shell did not park on the keyboard");

    // resize() hands back the descriptor; nothing else tells JS where the
    // grid is.
    regrid(60, 16, "resize returned no screen descriptor");

    let s = screen();
    if (s.magic !== SCREEN_MAGIC)
        fail(`screen magic is ${s.magic.toString(16)}, expected ${SCREEN_MAGIC.toString(16)}`);
    if (s.cols !== 60 || s.rows !== 16)
        fail(`resize(60, 16) gave ${s.cols}x${s.rows}`);

    // A resize repaints everything, and the host ticks to let it out.
    presented.length = 0;
    run(1);
    if (presented.length !== 1 || presented[0].w !== 60 || presented[0].h !== 16)
        fail(`the resize did not repaint the whole screen: ${JSON.stringify(presented)}`);

    // First boot: an empty store is unpacked without asking, and what came out
    // of the archive is what the shell was then found in.
    if (hasRootfs) {
        if (store.unpacks !== 1)
            fail(`the first boot unpacked ${store.unpacks} times, expected 1`);
        if (!store.files.has("/bin/sh"))
            fail("the unpack did not install /bin/sh");
        if (!store.dirs.has("/tmp") || !store.dirs.has("/import"))
            fail("boot did not make the directories the archive does not carry");
        const stamp = new TextDecoder().decode(store.files.get("/version") || new Uint8Array(0));
        if (!/^\d+\.\d+\.\d+/.test(stamp))
            fail(`/version reads ${JSON.stringify(stamp)}, expected a version`);
    }

    // init prints /share/motd before the shell, in green, and the prompt sets
    // its own colour rather than inheriting one. COLOR_GREEN is 2 and
    // COLOR_WHITE|COLOR_BRIGHT is 15, from the enum in src/kernel/screen.h.
    s = screen();
    // The geometry is /proc/host's alone. On the banner it would name the very
    // screen it is printed on, and go stale at the first resize.
    if (rows(s).some((line) => line.startsWith("screen:")))
        fail(`the banner reports the geometry: ${JSON.stringify(rows(s))}`);

    const motd_y = rows(s).findIndex((line) => line.startsWith("braam — a small operating system"));
    if (motd_y < 0)
        fail(`the motd did not print at boot: ${JSON.stringify(rows(s))}`);
    if (cell(s, 0, motd_y).fg !== 2)
        fail(`the motd is colour ${cell(s, 0, motd_y).fg}, expected green`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`the boot prompt is ${row(s, s.cursor_y)}, expected ${prompt()}`);
    // The directory is white on blue, the space beside it is not, and the $ is
    // bright white: three of one Sys::Echo's four runs, the fourth being the
    // reset. COLOR_WHITE is 7, COLOR_BLUE is 4 and COLOR_BLACK is 0.
    const dir_end = prompt().length - 2; // "home" is columns 0..3
    if (cell(s, 0, s.cursor_y).fg !== 7 || cell(s, 0, s.cursor_y).bg !== 4)
        fail(`the cwd is ${JSON.stringify(cell(s, 0, s.cursor_y))}, expected white on blue`);
    if (cell(s, dir_end, s.cursor_y).bg !== 0)
        fail(`the space before the $ is on ${cell(s, dir_end, s.cursor_y).bg}, expected black`);
    if (cell(s, dir_end + 1, s.cursor_y).fg !== 15 || cell(s, dir_end + 1, s.cursor_y).bg !== 0)
        fail(`the $ is ${JSON.stringify(cell(s, dir_end + 1, s.cursor_y))}, expected 15 on 0`);

    // M1's coverage, now supplied by the shell instead of by demo tasks:
    // `sleep` parks on the timer queue, and the delays tick reports are exact
    // because the clock is ours. It exercises argv and `exec` with it, since
    // `sleep` is a binary like everything else.
    type("sleep -m 30");
    press(KEY.ENTER);
    const delays = [1000, 1010, 1030].map((now) => run(now));
    const want_delays = [30, 20, -1];
    if (delays.join() !== want_delays.join())
        fail(`tick returned [${delays}], expected [${want_delays}]`);

    // M3, first criterion: `echo hello` prints and `help` lists the programs.
    s = submit("echo hello", 1040);
    if (!rows(s).includes("hello"))
        fail(`echo did not print: ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`the prompt after a success is ${row(s, s.cursor_y)}, expected ${prompt()}`);
}

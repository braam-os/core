// A pipeline in the shipping kernel, its status, and the quoting around it.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import {
    CTRL, fail, press, prompt, regrid, resize, row, rows, screen, submit,
} from "./harness.mjs";

export function check() {
    let s = screen();
    // M4, first criterion: a pipeline, in the shipping kernel. /bin is the
    // archive's binaries, and grep filters the listing, both running at once
    // over a bounded pipe. `clear` first, so the rows below are the
    // pipeline's and nothing else's.
    regrid(60, 16, "the resize before the pipeline failed");
    press("c".codePointAt(0), CTRL); // the "hi" typed above is still pending
    s = submit("clear", 1130);
    s = submit("ls /bin | grep tai", 1140);
    const listed = rows(s).filter((line) => line && !line.includes("$"));
    if (listed.join() !== "tail")
        fail(`ls /bin | grep tai printed ${JSON.stringify(listed)}, expected ["tail"]`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`a pipeline that matched left ${row(s, s.cursor_y)}, expected ${prompt()}`);

    // The status of a pipeline is its last command's: grep reports 1 when
    // nothing matched, and quote removal reaches argv on the way in.
    s = submit("ls /bin | grep zzz", 1150);
    if (!rows(s).includes(prompt(1)))
        fail(`an empty pipeline left ${row(s, s.cursor_y)}, expected ${prompt(1)}`);
    s = submit("echo 'a b' | wc", 1160);
    if (!rows(s).includes("1 2 4"))
        fail(`echo 'a b' | wc printed ${JSON.stringify(rows(s))}, expected 1 2 4`);

    // Two spaces: one would survive the word becoming two arguments, since
    // echo joins with a single space.
    s = submit("echo 'a  b'", 1161);
    if (!rows(s).includes("a  b"))
        fail(`echo 'a  b' printed ${JSON.stringify(rows(s))}, expected a  b`);
    s = submit("echo a\\ \\ b", 1162);
    if (!rows(s).includes("a  b"))
        fail(`echo a\\ \\ b printed ${JSON.stringify(rows(s))}, expected a  b`);
}

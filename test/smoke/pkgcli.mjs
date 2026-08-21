// /bin/pkg's subcommand table, before any of it does anything.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { fail, prompt, row, rows, screen, submit } from "./harness.mjs";

export function check() {
    let s = screen();
    // /bin/pkg's table (src/cmd/pkg/pkg.cpp). A bare `pkg` is a usage error
    // and 2, and a name no table row carries is the same. Every row is a
    // command now, so `is not built yet` has nothing left to say.
    s = submit("clear", 1184.7);
    s = submit("pkg", 1184.71);
    if (!rows(s).some((line) => line.startsWith("usage: pkg ")))
        fail(`pkg printed ${JSON.stringify(rows(s))}, expected a usage line`);
    // The names come off the table rather than a second list beside it. The
    // line is wider than this grid, so it is the start of it that is asserted.
    if (!rows(s).some((line) => line.startsWith("commands: autoremove ")))
        fail(`pkg's usage named no commands: ${JSON.stringify(rows(s))}`);
    if (!rows(s).includes(prompt(2)))
        fail(`pkg left ${row(s, s.cursor_y)}, expected ${prompt(2)}`);

    s = submit("clear", 1184.72);
    s = submit("pkg nonesuch", 1184.73);
    if (!rows(s).includes("pkg: unknown command: nonesuch"))
        fail(`pkg nonesuch printed ${JSON.stringify(rows(s))}`);
    if (!rows(s).includes(prompt(2)))
        fail(`pkg nonesuch left ${row(s, s.cursor_y)}, expected ${prompt(2)}`);

    // An operand is required, and one that is not a §6 token is the same
    // usage error rather than a name nothing provides.
    s = submit("clear", 1184.76);
    s = submit("pkg install", 1184.77);
    if (!rows(s).some((line) => line.startsWith("usage: pkg install ")))
        fail(`pkg install printed ${JSON.stringify(rows(s))}, expected a usage line`);
    if (!rows(s).includes(prompt(2)))
        fail(`pkg install left ${row(s, s.cursor_y)}, expected ${prompt(2)}`);
}

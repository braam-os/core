// /bin/pkg's subcommand table, before any of it does anything.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { fail, prompt, row, rows, submit } from "./harness.mjs";

// The second line of the block: `Usage:` is a heading and the command line is
// indented under it.
const USAGE = "    pkg <command> [<arg>...]";

// The block is taller than this grid, so what it printed is read through a
// pipe rather than off the screen; the status is read from the unpiped form,
// since a pipeline's is the last stage's.
function piped(line, now, want) {
    submit("clear", now);
    const s = submit(line, now + 0.001);
    if (!rows(s).includes(want))
        fail(`${line} printed ${JSON.stringify(rows(s))}, expected ${want}`);
    return s;
}

function status(line, now, want) {
    submit("clear", now);
    const s = submit(line, now + 0.001);
    if (!rows(s).includes(prompt(want)))
        fail(`${line} left ${row(s, s.cursor_y)}, expected ${prompt(want)}`);
}

export function check() {
    // /bin/pkg's table (src/cmd/pkg/pkg.cpp). A bare `pkg` is a usage error
    // and 2, and a name no table row carries is the same. Every row is a
    // command now, so `is not built yet` has nothing left to say.
    piped("pkg 2>&1 | head -n 2", 1184.7, USAGE);
    status("pkg", 1184.71, 2);

    // The rows come off the table rather than a second list beside it, each
    // with the operands its own usage line spells and one line of what it is
    // for.
    piped("pkg 2>&1 | grep \"install <package>\"", 1184.72,
        "    install <package>...  install packages and dependencies");

    piped("pkg nonesuch 2>&1 | head -n 1", 1184.73, "pkg: unknown command: nonesuch");
    status("pkg nonesuch", 1184.74, 2);

    // Asked for rather than provoked: the same block on stdout, and 0. `-h`
    // and `--help` are the same word before the table is searched.
    piped("pkg help | head -n 2", 1184.75, USAGE);
    status("pkg help", 1184.76, 0);
    piped("pkg -h | head -n 2", 1184.77, USAGE);
    piped("pkg --help | head -n 2", 1184.78, USAGE);

    // help takes no operand, and says so the way every other command does.
    piped("pkg help x 2>&1 | head -n 1", 1184.79, "Usage: pkg help");
    status("pkg help x", 1184.8, 2);

    // An operand is required, and one that is not a §6 token is the same
    // usage error rather than a name nothing provides.
    let s = submit("clear", 1184.81);
    s = submit("pkg install", 1184.82);
    if (!rows(s).some((line) => line.startsWith("Usage: pkg install ")))
        fail(`pkg install printed ${JSON.stringify(rows(s))}, expected a usage line`);
    if (!rows(s).includes(prompt(2)))
        fail(`pkg install left ${row(s, s.cursor_y)}, expected ${prompt(2)}`);
}

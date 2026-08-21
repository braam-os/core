// Control flow: if, for, while, and what `break 2` leaves behind.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { fail, rows, shows } from "./harness.mjs";

export function check() {
    const { line: vrun, has: vshows } = shows(1165.2, 0.01);
    // Control flow. The unit suite has the tree; what only a real shell shows
    // is that a loop rebinds its variable and that break and continue reach
    // the loop from inside a pipeline.
    vshows("for i in a b c; do echo $i; done", "a");
    vshows("for i in a b c; do echo $i; done", "c");
    vrun("clear");
    if (rows(vrun("for i in; do echo x; done")).includes("x"))
        fail("a for over an empty list ran its body");
    vrun("set p q");
    vshows("for i; do echo $i; done", "q"); // no `in`: the positional parameters
    vrun("set --");

    vshows("if true; then echo yes; else echo no; fi", "yes");
    vshows("if false; then echo no; else echo yes; fi", "yes");
    vshows("if false; then echo no; elif true; then echo yes; fi", "yes");
    vrun("clear");
    if (rows(vrun("if false; then echo no; fi")).includes("no"))
        fail("an if ran a branch its condition refused");

    // POSIX rather than v7: an if that takes no branch reports 0, where v7
    // leaves the failed condition's status behind.
    vshows("if false; then echo x; fi; echo $?", "0");
    vshows("for i in a; do false; done; echo $?", "1");
    vshows("while false; do echo x; done; echo $?", "0");

    vshows("while true; do echo once; break; done", "once");
    vshows("until false; do echo once; break; done", "once");
    vrun("clear");
    if (rows(vrun("for i in a b c; do continue; echo skipped; done")).includes("skipped"))
        fail("continue did not start the next turn");

    // `break 2` leaves both, and leaves nothing behind for the next line.
    // Spelled without spaces because the driver types a line in one burst and
    // the key ring holds 64: a human types slowly enough not to care.
    vrun("clear");
    let looped = rows(vrun("for i in a b;do for j in c d;do echo $i$j;break 2;done;done"));
    if (!looped.includes("ac"))
        fail(`a nested loop printed ${JSON.stringify(looped)}, expected ac`);
    if (looped.includes("ad") || looped.includes("bc"))
        fail(`break 2 left a loop running: ${JSON.stringify(looped)}`);
    vshows("echo after", "after");

    // Outside a loop both are silent no-ops, as they are in v7.
    vshows("break; echo still", "still");
    vshows("continue 3; echo still", "still");
}

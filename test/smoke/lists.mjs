// Lists, PS2 continuation, and a background job's own text.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { CTRL, fail, press, prompt, row, rows, run, screen, shows } from "./harness.mjs";

export function check() {
    const { at, line: vrun, has: vshows } = shows(1164.7, 0.01);
    // Lists. The unit suite has the tree; what only a real shell shows is that
    // each pipeline runs in its turn and that a status steers the next.
    vshows("echo one; echo two", "one");
    vshows("echo one; echo two", "two");
    vshows("true && echo yes", "yes");
    vshows("false || echo yes", "yes");
    vshows("false && echo a || echo b", "b");
    vshows("{ echo a; echo b; }", "b");
    vshows("! false && echo yes", "yes");
    vshows("echo kept # dropped", "kept");

    vrun("clear");
    if (rows(vrun("false && echo no")).includes("no"))
        fail("&& ran its right side after a failure");
    vrun("clear");
    if (rows(vrun("true || echo no")).includes("no"))
        fail("|| ran its right side after a success");

    // $? is the pipeline's, not the line's: both of these are one line.
    vshows("false; echo $?", "1");
    vshows("true; echo $?", "0");
    vshows("! true; echo $?", "1");

    // A half-typed construct asks for more, under PS2 rather than the prompt.
    vrun("clear");
    let cont = vrun("echo a &&");
    if (row(cont, cont.cursor_y) !== ">")
        fail(`a trailing && left ${JSON.stringify(row(cont, cont.cursor_y))}, expected >`);
    cont = vrun("echo b");
    if (!rows(cont).includes("a") || !rows(cont).includes("b"))
        fail(`the continuation printed ${JSON.stringify(rows(cont))}, expected a and b`);
    if (row(cont, cont.cursor_y) !== prompt())
        fail(`the continuation did not come back to a prompt`);

    // An unclosed group asks too, and PS2 is a variable.
    vrun("clear");
    vrun("PS2=... ");
    cont = vrun("{ echo in;");
    if (row(cont, cont.cursor_y) !== "...")
        fail(`PS2 left ${JSON.stringify(row(cont, cont.cursor_y))}, expected ...`);
    cont = vrun("}");
    if (!rows(cont).includes("in"))
        fail(`the group printed ${JSON.stringify(rows(cont))}, expected in`);
    vrun("unset PS2");

    // ^C at a continuation throws the accumulation away with the line.
    vrun("clear");
    vrun("echo lost &&");
    press("c".codePointAt(0), CTRL);
    run(at());
    cont = screen();
    if (row(cont, cont.cursor_y) !== prompt(130))
        fail(`^C on a continuation left ${JSON.stringify(row(cont, cont.cursor_y))}`);
    cont = vrun("echo fresh");
    if (rows(cont).includes("lost"))
        fail("^C on a continuation kept what had been typed");

    // A background job is listed by its own text, not by the whole line.
    vrun("clear");
    vrun("echo first; sleep -m 5000 &");
    cont = vrun("jobs");
    if (!rows(cont).some((line) => line.includes("running sleep -m 5000")))
        fail(`jobs after a list showed ${JSON.stringify(rows(cont))}`);
    if (rows(cont).some((line) => line.includes("running echo first")))
        fail("a list's first pipeline was filed as a job");
    vrun("kill %1");
    vrun("clear");
}

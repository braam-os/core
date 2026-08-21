// A program that takes the screen: the editor, the pager, and two claimants.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import {
    CTRL, KEY, fail, output, press, prompt, row, rows, run, screen, submit, type,
} from "./harness.mjs";

export function check() {
    let s = screen();
    // M7, first criterion: a full-screen editor opens a file, edits it, saves
    // it, and gives the shell's screen back on the way out.
    s = submit("clear", 3070);
    s = submit("edit /home/m7.txt", 3071);
    if (!rows(s).some((line) => line.startsWith(" /home/m7.txt")))
        fail(`edit drew no status line: ${JSON.stringify(rows(s))}`);
    if (row(s, 0) !== "")
        fail(`edit did not take the screen: ${JSON.stringify(rows(s))}`);

    type("hello");
    press(KEY.ENTER);
    type("editor");
    run(3072);
    s = screen();
    if (row(s, 0) !== "hello" || row(s, 1) !== "editor")
        fail(`typing did not reach the buffer: ${JSON.stringify(rows(s))}`);
    if (!rows(s).some((line) => line.startsWith(" /home/m7.txt *")))
        fail(`the modified flag never showed: ${JSON.stringify(rows(s))}`);

    // Editing what is already there: back to the start of the line, and a
    // character in the middle of it.
    press(KEY.HOME);
    press(KEY.UP);
    press(KEY.RIGHT);
    type("X");
    press("s".codePointAt(0), CTRL); // save
    run(3073);
    s = screen();
    if (row(s, 0) !== "hXello")
        fail(`the edit did not land: ${JSON.stringify(rows(s))}`);
    if (rows(s).some((line) => line.startsWith(" /home/m7.txt *")))
        fail(`the buffer is still modified after a save: ${JSON.stringify(rows(s))}`);

    press("q".codePointAt(0), CTRL); // quit
    run(3074);
    s = screen();
    if (row(s, s.cursor_y) !== prompt())
        fail(`edit did not give the screen back: ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3075);
    s = submit("cat /home/m7.txt", 3076);
    const saved = rows(s).filter((line) => line && !line.includes("$"));
    if (saved.join(",") !== "hXello,editor")
        fail(`the editor saved ${JSON.stringify(saved)}`);

    // A pager over a pipe: it reads its input to the end, then takes the keys.
    // The archive's README is the input because it is longer than the pane, which
    // is what makes PgDn mean anything.
    s = submit("clear", 3077);
    s = submit("cat /README | less", 3078);
    // Named once: the two checks below are the same row before and after a
    // scroll, and pinning the text twice let one of them go stale.
    const readme_top = "Braam is a small operating system in a browser tab.";
    if (!rows(s).some((line) => line.startsWith(" stdin ")))
        fail(`less drew no status line: ${JSON.stringify(rows(s))}`);
    if (row(s, 0) !== readme_top)
        fail(`less painted ${JSON.stringify(rows(s))}`);
    press(KEY.PAGE_DOWN);
    run(3079);
    s = screen();
    if (row(s, 0) === readme_top)
        fail("PgDn did not scroll the pager");
    press("q".codePointAt(0));
    run(3080);
    s = screen();
    if (row(s, s.cursor_y) !== prompt())
        fail(`less did not give the screen back: ${JSON.stringify(rows(s))}`);

    // Two pagers in a pipeline, and only one of them is one: the first stage's
    // output is a pipe, so it copies and claims nothing, and the second pages
    // what it was handed. Without that split the pair would deadlock on the
    // keyboard rather than reading as `cat`.
    s = submit("clear", 3061);
    s = submit("less /README | less", 3062);
    if (rows(s).some((line) => line === "less: no keyboard"))
        fail(`the first stage claimed the keyboard: ${JSON.stringify(rows(s))}`);
    if (row(s, 0) !== readme_top || !rows(s).some((line) => line.startsWith(" stdin ")))
        fail(`the second stage did not page the first's output: ${JSON.stringify(rows(s))}`);
    press("q".codePointAt(0));
    run(3063);
    s = screen();
    if (!row(s, s.cursor_y).endsWith("$"))
        fail(`the screen did not come back: ${JSON.stringify(rows(s))}`);

    // Two claimants at once, which spawning made natural: `edit` takes the
    // terminal whatever its output is, so a pager on the console beside it is
    // a pair that both want the keys. One holder, and the second asker is
    // refused — here the editor, which opens its file before it claims while
    // the pager claims first thing. Its message is on the shell's grid, under
    // the alternate screen until the pager gives that back.
    s = submit("clear", 3064);
    s = submit("edit /home/m7.txt | less", 3065);
    press("q".codePointAt(0));
    run(3066);
    s = screen();
    if (!rows(s).some((line) => line === "edit: no keyboard"))
        fail(`the second claimant was not refused: ${JSON.stringify(rows(s))}`);
    if (!row(s, s.cursor_y).endsWith("$"))
        fail(`the screen did not come back: ${JSON.stringify(rows(s))}`);
}

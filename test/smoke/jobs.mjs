// A job backgrounded, listed, brought back and killed.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import {
    CTRL, KEY, fail, press, prompt, row, rows, run, screen, submit, type,
} from "./harness.mjs";

export function check() {
    let s = screen();
    // M7, second criterion: a job is backgrounded and listed, and its finish
    // is announced at the next prompt.
    s = submit("clear", 3081);
    s = submit("sleep -m 5000 &", 3082);
    if (!rows(s).some((line) => line.startsWith("[1] ")))
        fail(`& said nothing: ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`& did not come back to a prompt: ${JSON.stringify(rows(s))}`);

    s = submit("jobs", 3083);
    if (!rows(s).some((line) => line.startsWith("[1]+ running sleep -m 5000")))
        fail(`jobs listed nothing: ${JSON.stringify(rows(s))}`);

    // There is no /proc/jobs any more: the table is the shell's own memory now
    // that the shell is a process, and no syscall shows one process another's.
    // The job's stages are still scheduler tasks, so /proc has a file each.
    s = submit("clear", 3084);
    s = submit("cat /proc/jobs", 3085);
    if (!rows(s).some((line) => line.includes("not found")))
        fail(`/proc/jobs still exists: ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3086);
    s = submit("cat /proc/meminfo", 3087);
    if (!rows(s).some((line) => line.startsWith("reserved ")))
        fail(`/proc/meminfo said nothing: ${JSON.stringify(rows(s))}`);

    // fg brings it back to the foreground and waits: the shell does not reach a
    // prompt until the job is done, and ^C reaches what fg adopted through fg's
    // own destructor. This was test_jobs's, until backgrounding a job that
    // stays running came to need a program the in-wasm tests cannot step.
    s = submit("clear", 3088);
    type("fg");
    press(KEY.ENTER);
    run(3089);
    s = screen();
    if (row(s, s.cursor_y) === prompt())
        fail(`fg came straight back to a prompt: ${JSON.stringify(rows(s))}`);
    press("c".codePointAt(0), CTRL);
    run(3090);
    s = screen();
    if (!rows(s).includes(prompt(130)))
        fail(`^C during fg left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);
    s = submit("jobs", 3091);
    if (rows(s).some((line) => line.includes("running")))
        fail(`^C during fg left the job running: ${JSON.stringify(rows(s))}`);
    s = submit("clear", 3091.4);
    s = submit("jobs", 3091.5);
    if (rows(s).some((line) => line.startsWith("[1]")))
        fail(`the killed job was never dropped: ${JSON.stringify(rows(s))}`);

    // And a job cancelled outright, which is the other half: kill %n reaches
    // every stage, and the shell stays where it was.
    s = submit("clear", 3092);
    s = submit("sleep -m 5000 &", 3093);
    s = submit("kill %2", 3094);
    s = submit("jobs", 3095);
    if (rows(s).some((line) => line.includes("running")))
        fail(`kill %2 left the job running: ${JSON.stringify(rows(s))}`);

    s = submit("clear", 3096);
    s = submit("sleep -m 5000 &", 3097);

    // The timer finally fires, and the job is reported and dropped.
    run(9000);
    s = submit("", 9001);
    if (!rows(s).some((line) => /^\[\d+\] done/.test(line)))
        fail(`the finished job was never announced: ${JSON.stringify(rows(s))}`);
    s = submit("clear", 9002);
    s = submit("jobs", 9003);
    if (rows(s).some((line) => /^\[\d+\]/.test(line)))
        fail(`the finished job is still listed: ${JSON.stringify(rows(s))}`);
}

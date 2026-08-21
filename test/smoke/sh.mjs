// /bin/sh as an ordinary program, with a shell in front of it.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import {
    CTRL, KEY, chdir, fail, output, press, prompt, row, rows, run, screen, submit, type, words,
} from "./harness.mjs";

export function check() {
    let s = screen();
    // The builtins are the shell's own state, so they are not files: `cd` is
    // not in /bin and never resolves through it.
    s = submit("clear", 9092);
    s = submit("ls /bin", 9093);
    if (words(s).includes("cd"))
        fail("cd is a builtin and must not be a file in /bin");
    // The second clause of builtin.h's rule keeps the file: a builtin shadows
    // the name at a prompt, not everywhere.
    for (const name of ["echo", "true", "false", "test"])
        if (!words(s).includes(name))
            fail(`${name} is a builtin but must still be a file in /bin`);
    if (!words(s).includes("timeout"))
        fail(`ls /bin lost a binary: ${JSON.stringify(output(s))}`);

    // A builtin is an ordinary pipeline stage, which is why `fg` can claim the
    // pump of the pipeline it is running in — so it pipes and redirects too.
    s = submit("clear", 9094);
    s = submit("jobs > /home/j", 9095);
    s = submit("cat /home/j", 9096);
    if (row(s, s.cursor_y) !== prompt())
        fail(`a redirected builtin left ${row(s, s.cursor_y)}, expected a bare prompt`);

    // /bin/sh: the shell as an ordinary program, running as a child of the
    // resident one. Everything below happens over the §4.3 syscall table —
    // Cursor for the prompt, KeyClaim for the keys, Pipe/Spawn/Wait for the
    // pipeline, Chdir for `cd` — and nothing in it is kernel code.
    s = submit("clear", 9200);
    s = submit("sh", 9201);
    // Two prompts on one screen: the resident shell's, with `sh` typed at it,
    // and the one the child drew for itself.
    // The child inherits the cwd, so both prompts read the same.
    if (!rows(s).includes(`${prompt()} sh`) || row(s, s.cursor_y) !== prompt())
        fail(`sh drew ${JSON.stringify(rows(s))}, expected its own prompt under "${prompt()} sh"`);

    // Its line editor: typing, Home, and a character inserted at the front.
    type("cho hi");
    press(KEY.HOME);
    type("e");
    press(KEY.ENTER);
    run(9202);
    s = screen();
    if (!rows(s).includes("hi"))
        fail(`sh's editor produced ${JSON.stringify(rows(s))}, expected hi`);

    // A pipeline of two real programs, built by a *process* out of Sys::Pipe
    // and two spawns, and a redirection it opened itself.
    s = submit("clear", 9203);
    s = submit("ls /bin | grep tail", 9204);
    if (!rows(s).includes("tail"))
        fail(`sh's pipeline printed ${JSON.stringify(rows(s))}, expected tail`);

    s = submit("echo written > /home/sh.out", 9205);
    s = submit("cat /home/sh.out", 9206);
    if (!rows(s).includes("written"))
        fail(`sh's redirection produced ${JSON.stringify(rows(s))}`);

    // Its own working directory, moved by its own builtin and inherited by
    // what it spawns — a child of a child of the resident shell.
    s = submit("clear", 9207);
    s = submit("cd /share", 9208);
    chdir("/share"); // the child's, not the resident shell's
    s = submit("pwd", 9209);
    if (!rows(s).includes("/share"))
        fail(`cd in sh left ${JSON.stringify(rows(s))}, expected /share`);

    // ^C reaches what sh put in front, and sh survives it: the whole point of
    // Sys::Fg. The prompt that comes back is sh's, reporting 130.
    s = submit("clear", 9210);
    type("sleep -m 60000");
    press(KEY.ENTER);
    run(9211);
    press("c".codePointAt(0), CTRL);
    run(9212);
    s = screen();
    if (!rows(s).some((line) => line.startsWith(prompt(130))))
        fail(`^C in sh left ${JSON.stringify(rows(s))}, expected sh's ${prompt(130)}`);

    // Cooked input reaches a child of sh: the pump cooks into the console, and
    // sh gave the console to `cat` by letting go of the keyboard.
    s = submit("clear", 9216.1);
    type("cat");
    press(KEY.ENTER);
    run(9216.2);
    type("typed");
    press(KEY.ENTER);
    run(9216.3);
    press("d".codePointAt(0), CTRL);
    run(9216.4);
    s = screen();
    if (rows(s).filter((line) => line === "typed").length !== 2)
        fail(`cat under sh echoed ${JSON.stringify(rows(s))}, expected the line twice`);

    // A background job, its table, and `kill %n` — all sh's own memory now,
    // over Sys::Spawn and Sys::Kill.
    s = submit("clear", 9216.5);
    s = submit("sleep -m 60000 &", 9216.6);
    if (!rows(s).some((line) => line.startsWith("[1] ")))
        fail(`sh did not announce the job: ${JSON.stringify(rows(s))}`);
    s = submit("jobs", 9216.7);
    if (!rows(s).some((line) => line.startsWith("[1]+ running sleep -m 60000")))
        fail(`sh's jobs printed ${JSON.stringify(rows(s))}`);
    s = submit("kill %1", 9216.8);
    if (!rows(s).some((line) => line.startsWith("[1] interrupt")))
        fail(`kill %n did not report the job: ${JSON.stringify(rows(s))}`);
    s = submit("clear", 9216.9);
    s = submit("jobs", 9216.95);
    if (rows(s).some((line) => line.includes("sleep")))
        fail(`kill %n left the job in the table: ${JSON.stringify(rows(s))}`);

    // A full-screen child claims the keyboard sh let go of, paints, and gives
    // it back — the claim transfer that made Sys::Fg necessary.
    s = submit("clear", 9217);
    type("less /README");
    press(KEY.ENTER);
    run(9218);
    s = screen();
    if (!rows(s).some((line) => line.includes("/README") && line.includes("q quits")))
        fail(`less under sh painted ${JSON.stringify(rows(s))}`);
    press("q".codePointAt(0));
    run(9219);
    s = screen();
    if (row(s, s.cursor_y) !== prompt())
        fail(`less did not give sh its screen back: ${JSON.stringify(rows(s))}`);

    // And ^C on one, which is the harder half: the claim is the kernel's, on the
    // killed process's record, so the shell has to get it back from a program
    // that never ran a line of its own cleanup.
    s = submit("clear", 9219.1);
    type("less /README");
    press(KEY.ENTER);
    run(9219.2);
    press("c".codePointAt(0), CTRL);
    run(9219.3);
    s = submit("echo alive", 9219.4);
    if (!rows(s).includes("alive"))
        fail(`sh lost the keyboard to a killed full-screen child: ${JSON.stringify(rows(s))}`);

    // And it leaves by its own builtin, back to the resident shell's prompt —
    // which is still where it was, because the `cd` above moved the child's
    // working directory and nobody else's (Concept.md §5.1).
    s = submit("exit", 9213);
    chdir("/home");
    s = submit("clear", 9214);
    s = submit("pwd", 9215);
    if (rows(s).includes("/share"))
        fail("sh's cd moved the resident shell's working directory");
    s = submit("echo back", 9216);
    if (!rows(s).includes("back"))
        fail(`the resident shell did not come back: ${JSON.stringify(rows(s))}`);
}

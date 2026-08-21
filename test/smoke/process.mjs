// M8: a program in an instance of its own, with a memory cap and a pid.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import {
    CTRL, KEY, fail, net, others, press, prompt, row, rows, run, screen, submit, type,
} from "./harness.mjs";

export function check() {
    let s = screen();
    // M8. Everything above this line already ran a program in an instance of
    // its own without saying so: `wc` is a binary in /bin now, and
    // `echo 'a b' | wc`, `wc < notes` and `curl /hello.txt | wc` are the
    // assertions M4, M5 and M6 wrote against the applet, unchanged. That is the
    // third criterion.

    // M8, first criterion: a program with a memory of its own, and a cap the
    // kernel set rather than the binary. hog takes everything it can and then
    // asks memory.grow for one page more.
    s = submit("clear", 9010);
    s = submit("hog", 9011);
    const hogged = rows(s).find((line) => line.startsWith("hog: pid "));
    if (!hogged)
        fail(`hog said nothing: ${JSON.stringify(rows(s))}`);
    if (!/^hog: pid \d+, took 1[0-9] MiB, memory is 256 pages$/.test(hogged))
        fail(`hog reported ${JSON.stringify(hogged)}`);
    if (!rows(s).includes("hog: memory.grow refused past the cap"))
        fail(`memory.grow was not capped: ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`hog exited ${row(s, s.cursor_y)}, expected a bare prompt`);

    // M8, second criterion: the pid a process sees is the one the host bound
    // into its import closure. Two runs are two processes, and neither can
    // name the other — the syscall has no argument for it (asserted against
    // the module's imports above).
    s = submit("clear", 9012);
    s = submit("hog", 9013);
    const again = rows(s).find((line) => line.startsWith("hog: pid "));
    if (!again || again === hogged)
        fail(`a second process reported the same pid: ${JSON.stringify(again)}`);

    // Two instances alive at once, each with sixteen megabytes that are
    // nobody else's, feeding one another through a kernel pipe.
    net.peak = 0;
    s = submit("clear", 9014);
    s = submit("tail -n 1 /share/motd | wc", 9015);
    if (!rows(s).some((line) => /^1 \d+ \d+$/.test(line)))
        fail(`a two-stage pipeline printed ${JSON.stringify(rows(s))}`);
    if (net.peak < 2)
        fail(`the pipeline peaked at ${net.peak} instances, expected 2`);

    // A process is an ordinary scheduler job: /proc lists it under argv[0],
    // and ^C reaches it through the pipe it is parked on. `wc` with no
    // argument reads its stdin, which nothing is going to write.
    type("wc");
    press(KEY.ENTER);
    if (run(9020) !== -1)
        fail("wc did not park on its stdin");
    s = submit("clear", 9021); // the ^C below needs the pipeline still running
    press("c".codePointAt(0), CTRL);
    if (run(9022) !== -1)
        fail("^C left the process scheduled");
    s = screen();
    if (!rows(s).includes(prompt(130)))
        fail(`^C on a process left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);
    if (others() !== 0)
        fail(`${others()} instances outlived their processes`);

    // And ^C on a pipeline, which is the harder half of Sys::Fg: the shell arms
    // its stages one at a time, having let the keyboard go before it spawned —
    // so by the second call something is in front and the caller owns neither
    // the keys nor what is there. Both stages have to be cancelled, or the
    // prompt comes back with one still reading.
    type("cat | wc");
    press(KEY.ENTER);
    if (run(9023) !== -1)
        fail("a two-stage pipeline did not park");
    if (others() !== 2)
        fail(`${others()} instances for a two-stage pipeline, expected 2`);
    s = submit("clear", 9024); // the ^C below needs the stages still running
    press("c".codePointAt(0), CTRL);
    if (run(9025) !== -1)
        fail("^C on a pipeline left the scheduler with work to do");
    s = screen();
    if (!rows(s).includes(prompt(130)))
        fail(`^C on a pipeline left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);
    if (others() !== 0)
        fail(`${others()} stages outlived a ^C on the pipeline`);

    // A file that is not a program is refused before anything runs, and says
    // so differently from a name that is not there at all.
    s = submit("clear", 9030);
    s = submit("/share/motd", 9031);
    if (!rows(s).some((line) => line.startsWith("/share/motd: not executable")))
        fail(`a non-binary was not refused: ${JSON.stringify(rows(s))}`);
    if (!rows(s).includes(prompt(126)))
        fail(`a non-binary left ${row(s, s.cursor_y)}, expected ${prompt(126)}`);

    // The archive's own #! script, run as a program: /bin/help is `#!/bin/sh`
    // over `less /share/help`, so this is an interpreter spawned by the kernel,
    // a shell that is not a job of its own, and a screen claimed by its child.
    // On a terminal it pages rather than printing, and q gives the screen back.
    s = submit("clear", 9040);
    s = submit("help", 9041);
    if (row(s, 0) !== "braam — the commands")
        fail(`help did not page the document: ${JSON.stringify(rows(s))}`);
    if (!rows(s).some((line) => line.startsWith(" /share/help ")))
        fail(`the pager named something else: ${JSON.stringify(rows(s))}`);
    press("q".codePointAt(0));
    run(9042);
    s = screen();
    if (row(s, s.cursor_y) !== prompt())
        fail(`help did not give the screen back: ${JSON.stringify(rows(s))}`);

    // ^C reaches both of them: the shell armed the script's pid, and the pager
    // it spawned is a child cancelled by its destructor rather than by a pid
    // anyone named.
    s = submit("help", 9043);
    press("c".codePointAt(0), CTRL);
    run(9044);
    s = screen();
    if (row(s, s.cursor_y) !== prompt(130))
        fail(`^C on a script left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);
    if (others() !== 0)
        fail(`${others()} instances outlived ^C on a #! script`);
}

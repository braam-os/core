// M9: a worker of its own, one that never returns, and a late reply.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import {
    CTRL, KEY, fail, kernel, net, others, press, prompt, row, rows, run, screen, submit, type,
} from "./harness.mjs";

export function check() {
    let s = screen();
    // M9. Every program runs in a worker of its own, the shell included, so
    // everything above this line already did — those assertions are M4's to
    // M8's, unedited, and this is the only line that notices. A latch on
    // "anything ran": the shell binds a worker at boot, before a command is
    // typed.
    if (!net.bound.length)
        fail("nothing bound a worker");

    // One end to end: getpid answered inside its own worker, a write relayed
    // back through the kernel, an exit status carried on the step that
    // reported it.
    net.terminated.length = 0;
    s = submit("clear", 9050);
    s = submit("spin 1", 9051);
    if (!rows(s).some((line) => /^spin: pid \d+, spinning briefly$/.test(line)))
        fail(`a program printed ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`spin 1 exited ${row(s, s.cursor_y)}, expected a bare prompt`);
    if (net.terminated.length !== 0)
        fail("a process that exited had its worker terminated rather than pooled");
    // Every hire this run has made, less the workers that went with the
    // processes it killed, less the one the shell is holding: 24, 22 and 1
    // here. The pool grows only for a pipeline wider than what is idle and
    // shrinks only on a kill, so spin's own is in it — put back rather than
    // terminated, which is the assertion above. The shell's is not: it is a
    // process for as long as the system is up, so one worker is out of the pool
    // from boot and each of the reboots above terminated the rest. The widest
    // moment is `help | cat`, four processes at once — the shell, the #!
    // script's interpreter, the pager it runs and cat — and the ^C on that
    // script above took a pair of those workers away again.
    if (net.proc.pooled() !== 1)
        fail(`the pool holds ${net.proc.pooled()} workers, expected one`);

    // M9, first criterion: a program that does not come back. The fake link
    // leaves its step undelivered, which is all the kernel ever sees of a real
    // loop — there is no reply, no timer, and nothing to cancel but the proxy.
    //
    // `net.hold(n)` counts binds from here (test/fakeworker.mjs), so what falls
    // between a hold and the command it was aimed at is what has to be counted.
    // The shell is not one of them: it binds at boot and at a respawn, never per
    // command. `clear` and a spawning program's own worker are, which is why
    // some of the holds below clear the screen first and some ask for the second.
    net.hold();
    type("spin");
    press(KEY.ENTER);
    if (run(9060) !== -1)
        fail("a spinning process left the kernel with work to do");
    if (others() !== 1)
        fail("spin did not reach an instance");

    s = submit("clear", 9061); // the ^C below needs the process still running
    press("c".codePointAt(0), CTRL);
    if (run(9062) !== -1)
        fail("^C left the process scheduled");
    s = screen();
    if (!rows(s).includes(prompt(130)))
        fail(`^C on a spinning process left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);
    if (net.terminated.length !== 1)
        fail(`${net.terminated.length} workers were terminated, expected 1`);
    if (others() !== 0)
        fail(`${others()} instances outlived their processes`);

    // The reply the terminated worker will never send, arriving anyway: it is
    // dropped, and the shell is none the wiser.
    net.release();
    run(9063);
    s = submit("echo after", 9064);
    if (!rows(s).includes("after"))
        fail(`the shell did not survive a killed process: ${JSON.stringify(rows(s))}`);

    // M9, second criterion: the shell keeps working while one is spinning.
    // Backgrounded, since a foreground job is waited for — what is being
    // asserted is that the kernel is free, not that the shell is rude.
    // The clear comes first: it is a program too, so a hold taken before it
    // would land on its worker rather than on the one that spins.
    s = submit("clear", 9070);
    net.hold();
    s = submit("spin &", 9071);
    const announced = rows(s).find((line) => /^\[\d+\] \d+$/.test(line));
    if (!announced)
        fail(`a backgrounded process did not announce itself: ${JSON.stringify(rows(s))}`);
    const job = announced.slice(1, announced.indexOf("]"));

    s = submit("echo alive", 9072);
    if (!rows(s).includes("alive"))
        fail(`the shell stalled behind a spinning process: ${JSON.stringify(rows(s))}`);
    s = submit("jobs", 9073);
    if (!rows(s).some((line) => line.startsWith(`[${job}]`) && line.includes(" running spin")))
        fail(`jobs did not list the spinning process: ${JSON.stringify(rows(s))}`);

    net.terminated.length = 0;
    s = submit(`kill %${job}`, 9074);
    if (net.terminated.length !== 1)
        fail(`kill terminated ${net.terminated.length} workers, expected 1`);
    if (others() !== 0)
        fail(`${others()} instances outlived kill %1`);
    net.release();
}

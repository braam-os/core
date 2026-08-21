// What a process does with its own children: wait, kill, pipe and cwd.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import {
    CTRL, KEY, chdir, fail, net, others, press, prompt, row, rows, run, screen, submit, type,
} from "./harness.mjs";

export function check() {
    let s = screen();
    // ---------------------------------------------------------- processes
    //
    // A program that starts a program. Everything below runs through Sys::Spawn,
    // Sys::Wait and Sys::Kill, and none of it could be a builtin.
    // The ordinary path: the child runs, writes to the stdio it shared with its
    // parent, and its status is what `timeout` reports.
    s = submit("clear", 9100);
    s = submit("timeout -m 10000 echo child", 9101);
    if (!rows(s).includes("child"))
        fail(`a spawned child printed ${JSON.stringify(rows(s))}, expected child`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`timeout over a fast child left ${row(s, s.cursor_y)}, expected a bare prompt`);
    if (others() !== 0)
        fail(`${others()} instances outlived timeout`);

    // The kill path: the child outlasts the delay, so the alarm task kills it
    // and 124 says which of the two ended it. The clock has to be moved past
    // the delay by hand, exactly as `sleep -m 30` above needs it.
    s = submit("clear", 9110);
    type("timeout -m 20 sleep -m 10000");
    press(KEY.ENTER);
    run(9111);
    if (others() !== 2)
        fail(`timeout over a slow child left ${others()} instances, expected 2`);
    run(9200); // past the delay: the alarm fires here
    s = screen();
    if (!rows(s).includes(prompt(124)))
        fail(`timeout did not fire: ${JSON.stringify(rows(s))}`);
    if (others() !== 0)
        fail(`${others()} instances outlived a fired timeout`);

    // The ownership chain, under the load it exists for: the child writes into
    // a pipe the *shell's* Job owns, and that block has to outlive both the
    // stage and every syscall server still parked on it.
    s = submit("clear", 9115);
    s = submit("timeout -m 10000 echo one two | wc", 9116);
    if (!rows(s).some((line) => line.trim() === "1 2 8"))
        fail(`a supervised child in a pipeline printed ${JSON.stringify(rows(s))}`);
    if (others() !== 0)
        fail(`${others()} instances outlived a supervised pipeline`);

    // A child's exit status reaches the parent, and the parent's reaches the
    // shell — two Waits deep, since `false` is a process of its own.
    s = submit("clear", 9120);
    s = submit("timeout -m 10000 false", 9121);
    if (!rows(s).includes(prompt(1)))
        fail(`a child's status did not reach the shell: ${JSON.stringify(rows(s))}`);

    // A name that is not a command is the parent's diagnostic, not a crash.
    s = submit("clear", 9130);
    s = submit("timeout -m 10000 nosuchthing", 9131);
    if (!rows(s).includes(prompt(127)))
        fail(`spawning a missing command gave ${JSON.stringify(rows(s))}, expected 127`);

    // A builtin is the shell's own state and cannot be spawned into a process.
    s = submit("clear", 9140);
    s = submit("timeout -m 10000 cd", 9141);
    if (!rows(s).some((line) => line.startsWith("timeout: cd:")))
        fail(`spawning a builtin gave ${JSON.stringify(rows(s))}, expected a refusal`);

    // The child inherits the parent's cwd, which the parent inherited from the
    // shell — the whole chain, asserted in one line.
    submit("cd /bin", 9150);
    chdir("/bin");
    s = submit("clear", 9151);
    s = submit("timeout -m 10000 pwd", 9152);
    if (!rows(s).includes("/bin"))
        fail(`a child's inherited cwd printed ${JSON.stringify(rows(s))}, expected /bin`);
    submit("cd /home", 9153);
    chdir("/home");

    // A pipe between a process and its child. `watch` moves the write end into
    // the child, so the child exiting is what closes the channel and gives the
    // read its end of input — nothing else ends that loop.
    s = submit("clear", 9145);
    type("watch -n 100 echo tick");
    press(KEY.ENTER);
    if (run(9146) !== 100000)
        fail("watch's -n was not read as seconds");
    if (!rows(screen()).includes("tick"))
        fail(`watch printed ${JSON.stringify(rows(screen()))}, expected tick`);
    if (others() !== 1)
        fail(`${others()} instances between rounds, expected watch alone`);
    press("c".codePointAt(0), CTRL);
    if (run(9147) !== -1)
        fail("^C on watch left the scheduler with work to do");
    s = screen();
    if (!rows(s).includes(prompt(130)))
        fail(`^C on watch left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);

    // timeout's default unit, and an interval that would not convert to ms.
    s = submit("clear", 9148);
    s = submit("timeout 10 echo child", 9148.1);
    if (!rows(s).includes("child"))
        fail(`timeout in seconds printed ${JSON.stringify(rows(s))}, expected child`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`timeout in seconds left ${row(s, s.cursor_y)}, expected a bare prompt`);
    s = submit("clear", 9148.2);
    s = submit("watch -n 4294968 echo tick", 9148.3);
    if (!rows(s).includes("usage: watch [-m] [-n <seconds>] <command> [<arg>...]"))
        fail(`a watch interval past the millisecond range gave ${JSON.stringify(rows(s))}`);
    if (others() !== 0)
        fail(`${others()} instances outlived watch`);

    // A child gets the worker kill through its parent, which is the proof that
    // a spawned process is an ordinary scheduler job.
    //
    // The pool has to be warm before the link is held: `spin` with no argument
    // never comes back, and a spawn that cannot be given a worker backs off and
    // asks again (§4) rather than running the loop anywhere the driver would
    // have to step it.
    s = submit("spin 1", 9155);
    if (net.proc.pooled() !== 2)
        fail(`the pool holds ${net.proc.pooled()} workers, expected two — 23 hired by`
             + " here, 20 terminated with the processes above that were killed, and one"
             + " held by the shell for as long as the system is up");

    net.terminated.length = 0;
    s = submit("clear", 9160);
    net.hold(2); // the parent binds a worker first, and it is the child that loops
    type("timeout -m 20 spin");
    press(KEY.ENTER);
    run(9161);
    if (others() !== 2)
        fail(`a supervised child left ${others()} instances, expected 2`);
    run(9200); // past the delay, so the kill is the alarm's and not ^C's
    net.release();
    run(9201);
    if (net.terminated.length !== 1)
        fail(`timeout over a child terminated ${net.terminated.length} workers, expected 1`);
    if (others() !== 0)
        fail(`${others()} instances outlived a killed child`);

    // ^C reaches a whole chain: the shell cancels the stage, the stage's End
    // cancels the child, and neither is left behind.
    s = submit("spin 1", 9205); // warm the pool again, for the reason above
    s = submit("clear", 9210);
    net.hold(2);
    type("timeout -m 100000 spin");
    press(KEY.ENTER);
    run(9211);
    if (others() !== 2)
        fail(`a supervised child left ${others()} instances, expected 2`);
    press("c".codePointAt(0), CTRL);
    if (run(9212) !== -1)
        fail("^C on a spawned pair left the scheduler with work to do");
    net.release();
    run(9213);
    if (others() !== 0)
        fail(`${others()} instances outlived ^C on a parent and its child`);
    s = submit("echo after", 9214);
    if (!rows(s).includes("after"))
        fail(`the shell did not survive ^C on a spawned pair: ${JSON.stringify(rows(s))}`);
}

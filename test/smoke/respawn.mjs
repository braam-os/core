// Workers taken away, and init putting the shell back.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import {
    CTRL, KEY, fail, kernel, mem, net, others, press, prompt, row, rows, run, screen, submit, type,
} from "./harness.mjs";

export function check() {
    let s = screen();
    // From here to `exit`, every case takes a worker away — and the shell is
    // one of the processes holding one, so each of them kills it and init
    // replaces what died (Concept.md §4). That bound is `RESPAWN_TRIES` deaths
    // inside `RESPAWN_FLOOR_MS` of *scheduler* time (src/user/boot.cpp), and
    // scheduler time here is whatever literal `run()` is passed. So the blocks
    // below are a second or more apart on that clock rather than a millisecond,
    // which is what keeps a shell that died from counting as a crash loop.
    // Anything inserted here needs the same spacing, or the session ends at
    // "the shell will not stay up" before `exit 7` is reached.

    // A worker taken away with a step still in it. `dropWorkers` is a host
    // letting go of every worker where `broke()` lets go of one link, and
    // either way the process has to be *failed* by whoever killed it: an
    // unanswered request the kernel is parked on is answered by nothing else.
    //
    // The shell holds a worker too now, so it goes with them and init starts
    // another — which is what makes this the strongest form of the case. Before
    // T8 a missed failure was a prompt that never came back; it is a whole
    // session that never comes back now, since the shell parked on the step it
    // was owed is the shell nobody will replace.
    s = submit("clear", 9075);
    net.hold();
    type("spin");
    press(KEY.ENTER);
    if (run(9075.1) !== -1)
        fail("a spinning process left the kernel with work to do");
    if (others() !== 1)
        fail("spin did not reach an instance");

    net.proc.dropWorkers();
    if (run(9075.2) !== -1)
        fail("dropping the workers left the kernel with work to do");
    s = screen();
    if (!rows(s).some((line) => line.startsWith("the shell died")))
        fail(`dropping the workers said ${JSON.stringify(rows(s))}`);
    // A bare prompt, not `spin`'s status: the shell that would have printed it
    // died in the same breath, and its replacement has no line to report.
    if (row(s, s.cursor_y) !== prompt())
        fail(`a dropped worker left ${row(s, s.cursor_y)}, expected a bare prompt`);
    if (others() !== 0)
        fail(`${others()} instances outlived the workers holding them`);
    net.release();

    // And the replacement got a worker, because `dropWorkers` lets go of the
    // workers it has rather than of the ability to make one: a host that can
    // still hire answers the next `exec` with one. There is no latch behind
    // that any more — the kernel is what paces the asking (Concept.md §4).
    s = submit("clear", 9075.3);
    s = submit("echo alive", 9075.4);
    if (!rows(s).includes("alive"))
        fail(`the shell after a dropped worker printed ${JSON.stringify(rows(s))}`);

    // The shell's own worker going away, which is the thing T8 risks rather
    // than a stand-in for it: init notices its child *died* rather than exited
    // and starts another (Concept.md §4). Killed from the host, since `kill`
    // refuses anything that is not a child of the caller.
    //
    // The shell's pid is init's — it runs inside init's task rather than a job
    // of its own — and /proc says so: a cwd is what only a program has. The tty
    // pump is spawned first, so init is 2.
    s = submit("clear", 11076);
    s = submit("cat /proc/2", 11076.1);
    if (!rows(s).some((line) => line.startsWith("name   init")))
        fail(`/proc/2 is not init: ${JSON.stringify(rows(s))}`);
    // Both memory figures are here, where a column would have been uniform: what
    // the instance holds, and the ceiling the kernel gave it.
    if (!rows(s).some((line) => /^mem    \d+$/.test(line)))
        fail(`/proc/2 does not report its memory: ${JSON.stringify(rows(s))}`);
    if (!rows(s).includes(`cap    ${256 * 65536}`))
        fail(`/proc/2 does not report its cap: ${JSON.stringify(rows(s))}`);
    if (!rows(s).some((line) => line.startsWith("cwd    /home")))
        fail(`/proc/2 has no cwd, so no process is there: ${JSON.stringify(rows(s))}`);

    // Killed with state on it: a job in its table, and a foreground armed. A
    // pipeline rather than one command, because a single child clears the
    // console on its way out and a stage does not — the shell clears it after
    // collecting, and that is the line the dead shell never reaches. It is
    // parked on Sys::Wait meanwhile, so it learns nothing until the stage it is
    // waiting for finishes, which is what the second run() is for.
    s = submit("clear", 11076.2);
    s = submit("sleep -m 60000 &", 11076.3);
    if (!rows(s).some((line) => /^\[\d+\] \d+$/.test(line)))
        fail(`the doomed shell did not announce the job: ${JSON.stringify(rows(s))}`);
    type("sleep -m 1 | wc");
    press(KEY.ENTER);
    run(11076.4);

    const live = net.proc.live();
    net.terminated.length = 0;
    net.proc.kill(2);
    if (net.proc.live() !== live - 1)
        fail("pid 2 is not the shell, so the case below would assert nothing");
    // The shell is a process like any other, so the kill reaches a worker
    // rather than merely dropping the record. It is parked on Sys::Wait with no
    // step outstanding, so this is the branch that has nothing to fail.
    if (net.terminated.length !== 1)
        fail(`killing the shell terminated ${net.terminated.length} workers, expected 1`);
    run(11078); // the timer fires, the child exits, and the shell steps to collect it

    s = screen();
    if (!rows(s).some((line) => line.startsWith("the shell died")))
        fail(`a shell whose worker went away said ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`the replacement shell left ${row(s, s.cursor_y)}, expected a bare prompt`);
    if (net.proc.live() !== 1)
        fail(`${net.proc.live()} processes after the replacement, expected the shell alone`);

    // ^C at its prompt, which is what init clearing the console foreground
    // buys, and it has to be the *first* thing the replacement is asked to do:
    // the pump cancels whatever is in front rather than asking whether it is
    // still alive, and any command run here would clear the stale set on its
    // way out. Without the clear the ^C is swallowed and the typed line
    // survives it, which is what the second half asserts.
    type("junk");
    press("c".codePointAt(0), CTRL);
    run(11079);
    s = screen();
    if (!rows(s).includes(prompt(130)))
        fail(`^C on the replacement left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);
    s = submit("echo clean", 11079.1);
    if (!rows(s).includes("clean"))
        fail(`the abandoned line was still in the editor: ${JSON.stringify(rows(s))}`);

    // A fresh shell, not the one that died: its table is empty, and what the
    // dead one backgrounded went with it — a process's children are cancelled
    // by its destructor.
    s = submit("clear", 11079.2);
    s = submit("jobs", 11079.3);
    if (rows(s).some((line) => line.includes("sleep")))
        fail(`the replacement inherited a job: ${JSON.stringify(rows(s))}`);

    // And the other half of the console: the replacement arming a foreground of
    // its own, and taking the screen back from a full-screen child.
    type("sleep -m 60000");
    press(KEY.ENTER);
    run(11079.6);
    press("c".codePointAt(0), CTRL);
    run(11079.7);
    s = screen();
    if (!rows(s).includes(prompt(130)))
        fail(`^C on the replacement's foreground left ${JSON.stringify(rows(s))}`);

    s = submit("clear", 11079.8);
    type("less /README");
    press(KEY.ENTER);
    run(11079.9);
    s = screen();
    if (!rows(s).some((line) => line.includes("/README") && line.includes("q quits")))
        fail(`less under the replacement painted ${JSON.stringify(rows(s))}`);
    press("q".codePointAt(0));
    run(11080);
    s = screen();
    if (row(s, s.cursor_y) !== prompt())
        fail(`less did not give the replacement its screen back: ${JSON.stringify(rows(s))}`);

    // The two ways a worker is not to be had, last of all, because both leave
    // the session in a state nothing after them should have to work around.
    //
    // A worker that is made and never loads its script. The kernel learns at
    // the first step, so the process reads as a crash — and nothing is latched
    // off by it: whether procworker.js loads is a question the host answers
    // afresh every time it is asked, which is what lets a host that recovers be
    // noticed (Concept.md §4).
    //
    // The pool is emptied by a *pipeline* rather than by `dropWorkers`, which
    // would take the shell's worker with it and make this a case about init.
    // One stage takes the last idle worker and the second has to hire, so the
    // second is the one that gets the broken link — and the shell keeps the
    // good worker it was already holding. The screen is cleared first, since
    // `clear` is a program too and would otherwise be a third claimant. Both
    // stages have to be programs — `echo` is a builtin and takes no worker.
    s = submit("clear", 13086);
    if (net.proc.pooled() !== 1)
        fail(`the pool holds ${net.proc.pooled()} workers, expected one before the break`);
    net.broken = true;
    s = submit("pwd | cat", 13087);
    if (!rows(s).some((line) => line.startsWith("/bin/cat: crashed")))
        fail(`a worker that never loaded printed ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== prompt(132))
        fail(`a crashed process left ${row(s, s.cursor_y)}, expected ${prompt(132)}`);
    if (others() !== 0)
        fail(`${others()} instances outlived a pipeline with a broken stage`);

    // And the next pipeline hires again and crashes the same way. That is the
    // latch's absence stated as an assertion: a host whose workers are born
    // broken is asked afresh rather than written off, and the cost of it is one
    // dead worker per hire until it recovers. A pipeline rather than a command,
    // because `pwd` gave its own worker back to the pool when it exited and a
    // single command would take that one and never hire.
    const made = net.links.length;
    s = submit("pwd | cat", 13089);
    if (row(s, s.cursor_y) !== prompt(132))
        fail(`a second pipeline after a broken worker left ${row(s, s.cursor_y)}`);
    if (net.links.length === made)
        fail("a broken worker stopped the next pipeline from hiring one");
    net.broken = false;

    // Where a worker cannot be made at all, the spawn *waits* for one: 10 ms,
    // then 20, 50, and so on to a second, saying so each time, until the host
    // can give one (Concept.md §4). There is no second place to run a process,
    // so this is the whole of what a host without workers gets.
    //
    // The shell is one of them and dies here for the last time: the drop takes
    // the worker it is holding, and the `exec` init answers with is the one
    // that waits. Everything after this line depends on it coming back.
    s = submit("clear", 13089.4);
    net.workers = false;
    net.proc.dropWorkers();
    net.bound.length = 0; // nothing may bind one from here until there is one

    // The kernel learns its shell is gone when it next tries to step it, and at
    // a prompt that is the next key: nothing is outstanding to be failed, since
    // the shell is parked on `key_read` and the *kernel* is holding that. So one
    // keystroke is spent provoking it, and it goes with the shell it reached.
    press("x".codePointAt(0));
    run(13089.5);
    s = screen();
    if (!rows(s).some((line) => line.startsWith("the shell died")))
        fail(`losing the workers said ${JSON.stringify(rows(s))}`);

    const WAIT_LINE = "/bin/sh: no worker, retrying";
    const waiting = (d) => rows(d).filter((line) => line === WAIT_LINE).length;
    if (waiting(s) !== 1)
        fail(`a host with no worker said ${JSON.stringify(rows(s))}`);

    // The backoff, on the clock the driver owns: 10 ms after the first refusal,
    // then 20 after the second. Nothing else is running, so each is a line.
    run(13099.6);
    run(13119.7);
    s = screen();
    if (waiting(s) !== 3)
        fail(`the spawn backed off ${waiting(s)} times, expected three`);
    if (net.bound.length !== 0)
        fail("a spawn with no worker to be had bound one anyway");

    // And it is a wait rather than a failure: the host finds a worker, the
    // shell that has been waiting for one starts, and the session is back.
    net.workers = true;
    run(13169.8);
    s = screen();
    if (row(s, s.cursor_y) !== prompt())
        fail(`a shell that waited for a worker left ${row(s, s.cursor_y)}`);
    s = submit("echo back", 13171);
    if (!rows(s).includes("back"))
        fail(`the shell after a wait printed ${JSON.stringify(rows(s))}`);
}

// ^C on a running pipeline, sleep's units, and a reply held back.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import {
    CTRL, KEY, fail, kernel, press, prompt, row, rows, run, screen, store, submit, type, words,
} from "./harness.mjs";

export function check() {
    let s = screen();
    // M4, second criterion: ^C interrupts a running pipeline and the prompt
    // comes back. tick's return value is what proves the sleep really went.
    type("sleep -m 5000");
    press(KEY.ENTER);
    if (run(1190) !== 5000)
        fail("the pipeline did not park on the timer");
    press("c".codePointAt(0), CTRL);
    if (run(1200) !== -1)
        fail("^C left the pipeline's timer armed");
    s = screen();
    if (!rows(s).includes(prompt(130)))
        fail(`^C on a pipeline left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);

    // The keyboard came back: the pump was its only receiver while the job
    // ran, and the shell has to be able to read it again afterwards.
    s = submit("echo back", 1210);
    if (!rows(s).includes("back"))
        fail(`the shell lost the keyboard after ^C: ${JSON.stringify(rows(s))}`);

    // The default unit: a bare number is seconds.
    type("sleep 5");
    press(KEY.ENTER);
    if (run(1211) !== 5000)
        fail("sleep's argument was not read as seconds");
    press("c".codePointAt(0), CTRL);
    if (run(1212) !== -1)
        fail("^C left the seconds timer armed");

    // A fraction has no parser, and a value that would not convert is refused.
    s = submit("clear", 1213);
    s = submit("sleep 0.5", 1214);
    if (!rows(s).some((line) => line.startsWith("usage: sleep")))
        fail(`a fractional sleep gave ${JSON.stringify(rows(s))}, expected the usage line`);
    if (!rows(s).includes(prompt(2)))
        fail(`a fractional sleep left ${row(s, s.cursor_y)}, expected ${prompt(2)}`);
    s = submit("clear", 1215);
    s = submit("sleep 4294968", 1216);
    if (!rows(s).some((line) => line.startsWith("usage: sleep")))
        fail(`a sleep past the millisecond range gave ${JSON.stringify(rows(s))}`);
    s = submit("clear", 1217);
    s = submit("sleep -m", 1218);
    if (!rows(s).some((line) => line.startsWith("usage: sleep")))
        fail(`-m without a number gave ${JSON.stringify(rows(s))}, expected the usage line`);

    // An asynchronous reply really does park the task: hold the reply back and
    // the shell has nothing pending until it lands, which is the path a browser
    // always takes and this harness otherwise short-circuits.
    store.defer = true;
    type("ls /home");
    press(KEY.ENTER);
    if (run(1220) !== -1)
        fail("a deferred storage reply left something scheduled");
    if (store.held.length === 0)
        fail("ls issued no storage request");
    store.defer = false;
    while (store.held.length)
        kernel().wake(store.held.shift(), 0, 0);
    run(1230);
    s = screen();
    if (!words(s).includes("notes"))
        fail(`the deferred listing never arrived: ${JSON.stringify(rows(s))}`);
}

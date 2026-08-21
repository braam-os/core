// A prompt line that wraps, ^L, and the scrollback chords.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import {
    CTRL, KEY, SHIFT, cell, fail, press, prompt, resize, row, rows, run, screen, shows, submit,
    type,
} from "./harness.mjs";

export function check() {
    let s = screen();
    // A prompt line long enough to wrap off the bottom of the screen, which is
    // the one path `Sys::Echo`'s `scrolled` replaced: the anchor's row goes up
    // *under* the write, and the editor has to follow it or every keystroke
    // after it repaints a row that has moved. A narrow, short grid rather than
    // a very long line, so the wrap is three rows and not thirty.
    // The count rather than the layout: a repaint that painted from an anchor
    // the scroll had moved would leave a stale row behind or blank a live one,
    // and either shows up here. Fewer keys than the 64-slot ring holds, since
    // `type` posts them all at once and `key()` refuses a full one.
    resize(20, 5);
    s = submit("clear", 9230);
    s = submit("echo a", 9230.1); // the anchor two rows down, so it survives
    const xs = (t) => (rows(t).join("").match(/x/g) || []).length;
    for (let i = 0; i < 56; i++)
        press("x".codePointAt(0));
    run(9230.2);
    s = screen();
    if (xs(s) !== 56)
        fail(`a line wrapped past the bottom shows ${xs(s)} of 56: `
             + JSON.stringify(rows(s)));
    if (s.cursor_y !== 4)
        fail(`the wrapped line left the cursor on row ${s.cursor_y}, expected the last`);

    // And a repaint of the same line after it has scrolled: backspace walks the
    // cursor back over two row boundaries and blanks the tail by hand, which it
    // can only do against an anchor that went up with the grid.
    for (let i = 0; i < 30; i++)
        press(KEY.BACKSPACE);
    run(9230.3);
    s = screen();
    if (xs(s) !== 26)
        fail(`backspacing a wrapped line left ${xs(s)} of 26: ${JSON.stringify(rows(s))}`);

    press("c".codePointAt(0), CTRL);
    run(9230.35);
    resize(60, 16);
    s = submit("clear", 9230.4);
    s = submit("echo narrow", 9230.5);
    if (!rows(s).includes("narrow"))
        fail(`the shell did not survive the narrow screen: ${JSON.stringify(rows(s))}`);

    // SYS_ECHO_FRESH: output that does not end its row leaves the cursor mid-
    // line, and the prompt must open one of its own rather than land beside it.
    s = submit("echo -n tail", 9230.6);
    if (!rows(s).some((line) => line === "tail"))
        fail(`echo -n did not get a row of its own: ${JSON.stringify(rows(s))}`);
    if (row(s, s.cursor_y) !== prompt())
        fail(`the prompt after echo -n is ${row(s, s.cursor_y)}, expected ${prompt()}`);

    // And the newline that opens that row goes out before any run's style, so a
    // scroll blanks the new bottom row in the default colour. It did not when
    // the newline rode with the first coloured run: screen.cpp's blank() takes
    // the sticky colour, so every prompt at the bottom of a full screen left a
    // blue bar from the $ to the right margin.
    resize(24, 4);
    s = submit("clear", 9230.7);
    for (let i = 0; i < 6; i++)
        s = submit("echo -n filling", 9230.71 + i / 100);
    for (let x = prompt().length; x < s.cols; x++)
        if (cell(s, x, s.cursor_y).bg !== 0)
            fail(`column ${x} past the prompt is on ${cell(s, x, s.cursor_y).bg}, expected black`);
    resize(60, 16);
    s = submit("clear", 9230.8);

    // ^L: the screen goes, the prompt is redrawn at the top with the line still
    // on it, and the cursor lands after what was typed. That last part used to
    // be a cursor_set of its own after the prompt; SYS_ECHO_END is what places
    // it now, and this is the case that says so.
    s = submit("echo scrollback", 9230.9);
    type("keep me");
    run(9230.91);
    press("l".codePointAt(0), CTRL);
    run(9230.92);
    s = screen();
    if (rows(s).some((line) => line.includes("scrollback")))
        fail(`^L left the screen behind: ${JSON.stringify(rows(s))}`);
    if (s.cursor_y !== 0)
        fail(`^L drew the prompt on row ${s.cursor_y}, expected the top`);
    if (row(s, 0) !== `${prompt()} keep me`)
        fail(`^L left ${JSON.stringify(row(s, 0))}, expected the line`);
    if (s.cursor_x !== prompt().length + 1 + "keep me".length)
        fail(`^L left the cursor at column ${s.cursor_x}`);
    press("c".codePointAt(0), CTRL);
    run(9230.93);

    // Scrollback. Sixteen echoes on a sixteen-row screen put seventeen rows off
    // the top, so two presses of half a screen land the first of them on the
    // top row. `s.cells` is a composed block while a view is up, which is why
    // the descriptor is re-read after every one of these.
    s = submit("clear", 9230.94);
    for (let i = 0; i < 16; i++)
        s = submit(`echo line${i}`, 9230.95 + i / 10000);
    const bottom = rows(s).join("\n");
    if (bottom.includes("line0"))
        fail(`line0 was meant to have scrolled off: ${JSON.stringify(rows(s))}`);

    press(KEY.PAGE_UP, SHIFT);
    press(KEY.PAGE_UP, SHIFT);
    run(9230.96);
    s = screen();
    if (row(s, 0) !== "line0")
        fail(`Shift+PageUp put ${JSON.stringify(row(s, 0))} on top, expected line0`);
    if (s.cursor_on !== 0)
        fail("the cursor is drawn over the scrollback");

    // A row at a time is the same chord with an arrow — what a wheel notch
    // becomes (web/worker.js) — and the shell is holding the keys, so this is
    // also the pump intercepting it ahead of a claimant.
    const back = rows(s);
    press(KEY.DOWN, SHIFT);
    run(9230.961);
    s = screen();
    if (row(s, 0) !== back[1])
        fail(`Shift+Down put ${JSON.stringify(row(s, 0))} on top, expected ` +
             `${JSON.stringify(back[1])}`);
    press(KEY.UP, SHIFT);
    run(9230.962);
    s = screen();
    if (row(s, 0) !== "line0")
        fail(`Shift+Up put ${JSON.stringify(row(s, 0))} on top, expected line0`);

    // Back down again, onto exactly the screen that was left behind.
    press(KEY.PAGE_DOWN, SHIFT);
    press(KEY.PAGE_DOWN, SHIFT);
    run(9230.97);
    s = screen();
    if (rows(s).join("\n") !== bottom)
        fail(`Shift+PageDown did not restore the screen: ${JSON.stringify(rows(s))}`);
    if (!s.cursor_on)
        fail("the cursor did not come back with the live screen");

    // And any other key is the way back, the keystroke itself still landing.
    press(KEY.PAGE_UP, SHIFT);
    run(9230.98);
    type("x");
    run(9230.99);
    s = screen();
    if (row(s, s.cursor_y) !== `${prompt()} x`)
        fail(`typing did not leave the scrollback: ${JSON.stringify(rows(s))}`);
    press("c".codePointAt(0), CTRL);
    run(9230.995);

    // A program holding the screen keeps the chord: less pages its own grid,
    // and the console's history is not the one on screen.
    s = submit("clear", 9230.996);
    type("less /README");
    press(KEY.ENTER);
    run(9230.997);
    press(KEY.PAGE_UP, SHIFT);
    run(9230.998);
    s = screen();
    if (!rows(s).some((line) => line.includes("/README") && line.includes("q quits")))
        fail(`Shift+PageUp took the screen from less: ${JSON.stringify(rows(s))}`);
    press("q".codePointAt(0));
    run(9230.999);
    s = submit("echo alive", 9230.9995);
    if (!rows(s).includes("alive"))
        fail(`the shell did not survive the pager: ${JSON.stringify(rows(s))}`);
}

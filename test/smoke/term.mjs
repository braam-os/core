// The grid as the page sees it: selection, paste, line editing, repaint accounting.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { pasted } from "../../web/keys.js";
import { Renderer } from "../../web/render.js";
import {
    CTRL, KEY, cell, fail, gridAddr, kernel, mem, net, presented, press, prompt, regrid,
    resetTicks, resize, row, rows, run, screen, submit, ticks, type,
} from "./harness.mjs";

export function check() {
    let s = screen();
    // The renderer's selection, read off the real grid rather than a mock of
    // one. A drag names cells in device pixels and the text it gives back is
    // what the page puts on the clipboard; none of it reaches the kernel
    // (Concept.md §3.5), so this is the only check web/render.js gets.
    {
        let ground = []; // the background each cell of a painted row got
        const ctx = {
            measureText: (t) => ({
                width: 8 * [...t].length,
                fontBoundingBoxAscent: 12,
                fontBoundingBoxDescent: 4,
            }),
            fillRect: () => ground.push(ctx.fillStyle),
            fillText: () => {},
        };
        const r = new Renderer({ getContext: () => ctx }, mem, {});
        r.attach(gridAddr());

        const y = rows(s).indexOf("hello");
        const paint = () => {
            ground = [];
            r.present(0, y, s.cols, 1);
            return ground;
        };
        const plain = paint();

        r.select("start", 0, y * r.cellH);
        r.select("move", 4 * r.cellW, y * r.cellH);
        r.select("end", 4 * r.cellW, y * r.cellH);
        if (r.text() !== "hello")
            fail(`the renderer selected ${JSON.stringify(r.text())}, expected "hello"`);

        // And the five cells it named, and no others, are painted reversed.
        const swapped = paint().reduce((n, bg, i) => n + (bg !== plain[i] ? 1 : 0), 0);
        if (swapped !== 5)
            fail(`the selection reversed ${swapped} cells, expected 5`);

        // Dragging the other way names the same cells: the anchor and the head
        // are put in reading order, not in the order they were made.
        r.select("start", 4 * r.cellW, y * r.cellH);
        r.select("end", 0, y * r.cellH);
        if (r.text() !== "hello")
            fail(`a backwards drag selected ${JSON.stringify(r.text())}`);

        // A drag off the edges clamps into the grid, and select-all names the
        // same cells without a drag at all. Neither hands back the blank rows
        // below the last line of output, and neither keeps a trailing blank.
        const screen = rows(s).join("\n").replace(/\n+$/, "");
        r.select("start", -99, -99);
        r.select("end", 1e6, 1e6);
        if (r.text() !== screen)
            fail(`a full-screen drag gave ${JSON.stringify(r.text())}`);
        if (r.text().endsWith("\n") || r.text().includes(" \n"))
            fail("a full-screen drag kept blanks the screen only pads with");

        r.clear();
        r.all();
        if (r.text() !== screen)
            fail(`select-all gave ${JSON.stringify(r.text())}`);

        // A click is not a selection: it clears, so ^C stays an interrupt.
        r.select("start", 8, 8);
        r.select("end", 9, 9);
        if (r.text() !== "" || r.clear())
            fail("a click left a selection behind");
    }

    // The other half of the page's clipboard: a paste is a run of keystrokes
    // and nothing else (Concept.md §3.5). web/keys.js turns the text into them
    // — one Enter for a newline however it is spelled, and no key at all for a
    // control character that no key produces.
    if (pasted("a\r\nb\tc").join() !== [97, KEY.ENTER, 98, KEY.TAB, 99].join())
        fail(`pasted() gave [${pasted("a\r\nb\tc")}]`);

    // The document is 148 lines, so the grid has to hold all of it or the
    // headings scroll off the top. SCREEN_MAX_ROWS is 256.
    submit("clear", 1045);
    regrid(100, 160, "the resize before help failed");
    // M3, first criterion. `help` is a #! script over `less`, and `less` off a
    // terminal is a `cat`, so the document reaches a pipe unchanged. That it
    // names every builtin and every binary is asserted against the archive
    // above; what is asserted here is that the command runs at all.
    s = submit("help | cat", 1050);
    for (const heading of ["Shell builtins", "Programs in /bin"])
        if (!rows(s).includes(heading))
            fail(`help printed no ${heading} section: ${JSON.stringify(rows(s))}`);
    for (const name of ["cd", "echo", "export", "trap", "cat", "help", "less", "ls", "wc"])
        if (!rows(s).some((line) => line.startsWith(`  ${name} `)))
            fail(`help did not list ${name}: ${JSON.stringify(rows(s))}`);

    // A pasted line is longer than the 64-slot key ring, so it is fed at the
    // rate the console drains it: key() says whether it took the keystroke, and
    // the rest of the run waits for the tick that empties the ring. This is the
    // loop in web/worker.js, and pushing the run in one go would drop its tail.
    // The screen is wide here, so the echoed line does not wrap.
    {
        const codes = pasted(`echo ${"z".repeat(80)}\r\n`);
        let at = 0, turns = 0;
        while (at < codes.length && turns++ < 40) {
            while (at < codes.length && kernel().key(codes[at], 0))
                at++;
            run(1055);
        }
        if (at !== codes.length)
            fail(`the paste stalled after ${at} of ${codes.length} keystrokes`);
        if (turns < 2)
            fail(`the ring took ${codes.length} keystrokes at once, so nothing was paced`);
        s = screen();
        if (!rows(s).includes("z".repeat(80)))
            fail(`the pasted line did not run: ${JSON.stringify(rows(s))}`);
        if (row(s, s.cursor_y) !== prompt())
            fail(`the paste left ${row(s, s.cursor_y)}, expected a fresh prompt`);
    }

    regrid(60, 16, "the resize after help failed");

    // M3, second criterion, first half: a nonzero exit code is observable —
    // the shell carries it in the next prompt.
    s = submit("false", 1060);
    if (!rows(s).includes(prompt(1)))
        fail(`a failing program left ${row(s, s.cursor_y)}, expected ${prompt(1)}`);
    // And it is red, ahead of the cwd on blue and the bright white $: three of
    // the four runs one Sys::Echo carries. COLOR_RED is 1.
    if (cell(s, 0, s.cursor_y).fg !== 1)
        fail(`the status is colour ${cell(s, 0, s.cursor_y).fg}, expected red`);
    if (cell(s, 4, s.cursor_y).bg !== 4)
        fail(`the cwd after a status is on ${cell(s, 4, s.cursor_y).bg}, expected blue`);
    const dollar = prompt(1).length - 1;
    if (cell(s, dollar, s.cursor_y).fg !== 15)
        fail(`the $ after a status is colour ${cell(s, dollar, s.cursor_y).fg}, expected white`);

    s = submit("nosuch", 1070);
    if (!rows(s).some((line) => line.startsWith("nosuch: not found")))
        fail(`an unknown command said nothing: ${JSON.stringify(rows(s))}`);
    if (!rows(s).includes(prompt(127)))
        fail(`an unknown command left ${row(s, s.cursor_y)}, expected ${prompt(127)}`);

    s = submit("true", 1080);
    if (row(s, s.cursor_y) !== prompt())
        fail(`a succeeding program left ${row(s, s.cursor_y)}, expected ${prompt()}`);

    // M3, second criterion, second half: Up recalls history, Home reaches the
    // start of the recalled line, and ^C abandons it.
    press(KEY.UP);
    run(1090);
    s = screen();
    if (!row(s, s.cursor_y).endsWith("true"))
        fail(`Up recalled ${row(s, s.cursor_y)}, expected it to end in true`);

    press(KEY.HOME);
    run(1100);
    s = screen();
    // The anchor is one past the prompt, whose trailing space row() trims.
    const home_x = prompt().length + 1;
    if (s.cursor_x !== home_x)
        fail(`Home left the cursor at column ${s.cursor_x}, expected ${home_x}`);

    press("c".codePointAt(0), CTRL);
    run(1110);
    s = screen();
    if (!rows(s).includes(prompt(130)))
        fail(`^C left ${row(s, s.cursor_y)}, expected ${prompt(130)}`);

    // M2's coverage: at most one present per tick, and between them they cover
    // every cell the editor drew and the cell the cursor left — that one has to
    // repaint or it ghosts.
    //
    // The editor is a program (Concept.md §4), so every operation it makes is a
    // step of its own; what M2 asked for was one present per *tick*, and that is
    // still exactly what happens. Since Sys::Echo carries the whole repaint,
    // both a keystroke and a whole prompt are one tick.
    presented.length = 0;
    resetTicks();
    const x0 = s.cursor_x;
    const y0 = s.cursor_y;
    type("hi");
    run(1120);
    s = screen();
    if (s.cursor_x !== x0 + 2)
        fail(`the cursor is at column ${s.cursor_x}, expected ${x0 + 2}`);
    if (!presented.length || presented.length > ticks())
        fail(`${presented.length} presents over ${ticks()} ticks`);
    const r = presented.reduce((a, b) => ({
        x: Math.min(a.x, b.x),
        y: Math.min(a.y, b.y),
        w: Math.max(a.x + a.w, b.x + b.w) - Math.min(a.x, b.x),
        h: Math.max(a.y + a.h, b.y + b.h) - Math.min(a.y, b.y),
    }));
    if (r.x > x0 || r.y > y0 || r.x + r.w < s.cursor_x + 1 || r.y + r.h <= y0)
        fail(`presents ${r.x},${r.y} ${r.w}x${r.h} miss ${x0}..${s.cursor_x},${y0}`);

    // §4.4's cost is per operation, so the count is the measurement — and it is
    // measured rather than read off the source, where a wrapper that grew a
    // call would not show. Both spans run from one parked key_read to the next.
    // A keystroke is the Echo that repaints and that key_read. Enter is that
    // Echo, the newline ending the row, the cwd the next prompt names, the Echo
    // that draws it, and the key_read: the directory is asked for every line on
    // purpose, since a stale prompt is believed.
    const calls = () => net.proc.stats().calls;
    let was = calls();
    type("x");
    run(1121);
    if (calls() - was !== 2)
        fail(`a keystroke costs ${calls() - was} round trips, expected 2`);

    press("u".codePointAt(0), CTRL); // an empty line, so the count is the prompt's
    run(1122);
    was = calls();
    press(KEY.ENTER);
    run(1123);
    if (calls() - was !== 5)
        fail(`Enter to the next prompt costs ${calls() - was} round trips, expected 5`);
    s = screen();
    if (row(s, s.cursor_y) !== prompt())
        fail(`an empty line left ${row(s, s.cursor_y)}, expected ${prompt()}`);
    type("hi"); // what the resize below reflows
    run(1124);

    // M2's second criterion: resize reflows, keeping the rows in use.
    regrid(20, 2, "the reflowing resize failed");
    s = screen();
    if (s.cols !== 20 || s.rows !== 2)
        fail(`resize(20, 2) gave ${s.cols}x${s.rows}`);
    if (!row(s, s.cursor_y).endsWith("hi"))
        fail(`the reflow lost the line being edited: ${row(s, s.cursor_y)}`);
    if (s.cursor_y >= s.rows)
        fail(`the cursor left the grid at ${s.cursor_x},${s.cursor_y}`);

    // A geometry the kernel will not honour is clamped, and reported back.
    s = regrid(9999, 9999, "an oversized resize was refused");
    if (s.cols !== 512 || s.rows !== 256)
        fail(`an oversized resize gave ${s.cols}x${s.rows}`);
}

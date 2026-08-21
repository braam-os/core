// The working directory, and the redirections that resolve against it.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import {
    CTRL, KEY, chdir, fail, press, prompt, row, rows, run, screen, submit, type,
} from "./harness.mjs";

export function check() {
    let s = screen();
    // M5: the shell starts in /home, which is where a redirection lands.
    // `pwd` reads its own cwd through Sys::Chdir now, so this is also the proof
    // that a top-level command inherits the shell's rather than starting at /.
    s = submit("clear", 1165);
    s = submit("pwd", 1166);
    if (!rows(s).includes("/home"))
        fail(`pwd printed ${JSON.stringify(rows(s))}, expected /home`);

    // ...and it follows `cd`, since that is what a process inherits.
    submit("cd /bin", 1167);
    chdir("/bin");
    s = submit("clear", 1168);
    s = submit("pwd", 1169);
    if (!rows(s).includes("/bin"))
        fail(`pwd after cd printed ${JSON.stringify(rows(s))}, expected /bin`);

    // The prompt names the basename, so the root is the one directory with no
    // name of its own — path_basename answers "/" for it rather than nothing.
    s = submit("cd /", 1169.1);
    chdir("/");
    if (row(s, s.cursor_y) !== "/ $" || row(s, s.cursor_y) !== prompt())
        fail(`cd / left ${row(s, s.cursor_y)}, expected "/ $"`);

    // A relative path from a program resolves against that inherited cwd, not
    // against the root: `ls .` in /bin has to find the binaries.
    submit("cd /bin", 1169.15);
    chdir("/bin");
    s = submit("clear", 1169.2);
    s = submit("ls . | grep wc", 1169.4);
    if (!rows(s).includes("wc"))
        fail(`ls . in /bin printed ${JSON.stringify(rows(s))}, expected wc`);
    submit("cd /home", 1169.6);
    chdir("/home");

    // M5, first criterion, first half: a redirection that really writes, an
    // append that follows it, and a file argument that reads it back.
    submit("echo one > notes", 1170);
    submit("echo two >> notes", 1171);
    s = submit("clear", 1172);
    s = submit("cat notes", 1173);
    const notes = rows(s).filter((line) => line && !line.includes("$"));
    if (notes.join(",") !== "one,two")
        fail(`cat notes printed ${JSON.stringify(notes)}, expected one,two`);

    // One file named twice. §5.2 used to refuse the second open outright; Input
    // now opens the second only after closing the first.
    s = submit("clear", 1173.1);
    s = submit("cat notes notes", 1173.2);
    const twice = rows(s).filter((line) => line && !line.includes("$"));
    if (twice.join(",") !== "one,two,one,two")
        fail(`cat notes notes printed ${JSON.stringify(twice)}, expected one,two,one,two`);

    // Two descriptors on one file at the same moment, which laziness alone does
    // not fix: the shell opens notes for the stage's stdin and Sys::Spawn moves
    // that handle into grep, which then opens notes again for itself.
    s = submit("clear", 1173.3);
    s = submit("grep one notes < notes", 1173.4);
    if (!rows(s).includes("one"))
        fail(`grep with a redirection on its own file printed ${JSON.stringify(rows(s))}`);

    // A redirection that cannot be opened stops the command before it runs.
    // /proc is the read-only mount now that the store holds everything else.
    s = submit("echo hi > /proc/uptime", 1174);
    if (!rows(s).some((line) => line.startsWith("/proc/uptime: ")))
        fail(`a read-only redirection said nothing: ${JSON.stringify(rows(s))}`);
    if (!rows(s).includes(prompt(1)))
        fail(`a refused redirection left ${row(s, s.cursor_y)}, expected ${prompt(1)}`);

    // Coverage that used to live in test_shell, moved here when its programs
    // became binaries: a filter stopping early, a three-stage pipeline, and
    // typing into a running job's stdin with ^D as end of input.
    s = submit("clear", 1174.1);
    submit("mkdir /home/d", 1174.2);
    submit("touch /home/d/a /home/d/b /home/d/c", 1174.3);
    s = submit("clear", 1174.4);
    s = submit("ls /home/d | head -n 2", 1174.5);
    const cut = rows(s).filter((line) => line && !line.includes("$"));
    if (cut.join(",") !== "a,b")
        fail(`head did not stop the producer: ${JSON.stringify(cut)}`);

    s = submit("clear", 1174.6);
    s = submit("ls /home/d | grep b | head -n 1", 1174.7);
    const three = rows(s).filter((line) => line && !line.includes("$"));
    if (three.join(",") !== "b")
        fail(`a three-stage pipeline printed ${JSON.stringify(three)}`);

    // stdin is the pump's other job: what is typed reaches a running program,
    // echoed once by the pump and printed again by cat, and ^D ends the input.
    s = submit("clear", 1174.8);
    type("cat");
    press(KEY.ENTER);
    run(1174.9);
    // Longer than the input pipe has slots, which is the point: the pump sends
    // a cooked line as one chunk, and used to send one per keystroke and drop
    // the rest of the line once the eight slots were full. An applet drained
    // the pipe in the same tick and never showed it; a process reads one
    // syscall at a time and always would.
    type("a longer line than eight");
    press(KEY.ENTER);
    run(1175.0);
    s = screen();
    if (rows(s).filter((line) => line === "a longer line than eight").length !== 2)
        fail(`typing into cat printed ${JSON.stringify(rows(s))}, expected it twice`);
    press("d".codePointAt(0), CTRL);
    run(1175.1);
    s = screen();
    if (row(s, s.cursor_y) !== prompt())
        fail(`^D did not end cat's input: ${JSON.stringify(rows(s))}`);

    submit("rm -r /home/d", 1175.2);

    // `<` and `2>` both reach the filesystem too.
    s = submit("clear", 1175);
    s = submit("wc < notes", 1176);
    if (!rows(s).some((line) => line.startsWith("2 2 8")))
        fail(`wc < notes printed ${JSON.stringify(rows(s))}, expected 2 2 8`);
    submit("cat nosuchfile 2> err", 1177);
    s = submit("clear", 1178);
    s = submit("cat err", 1179);
    if (!rows(s).some((line) => line.startsWith("cat: nosuchfile: not found")))
        fail(`2> did not capture a diagnostic: ${JSON.stringify(rows(s))}`);
}

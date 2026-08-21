// S10: the language all at once. Last, because it exits the shell.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { fail, net, output, prompt, row, rows, screen, shows, store, submit } from "./harness.mjs";

export function check() {
    let s = screen();
    const plant = (path, text) => store.files.set(path, new TextEncoder().encode(text));
    // S10: the language all at once. Every block above stays inside one stage;
    // what is left to check is that they compose, which is what a script does.
    const t = shows(14300, 0.005);
    // A line the driver types must fit the 64-character key ring, so anything
    // longer is run out of a file instead — which is what a script is for.
    const ifile = (line, path, want) => {
        submit(line, t.at());
        submit("clear", t.at());
        const got = output(submit("cat " + path, t.at())).join("|");
        if (got !== want)
            fail(`\`${line}\` left ${JSON.stringify(got)}, expected ${JSON.stringify(want)}`);
    };

    submit("mkdir /home/s10", t.at(0.01));
    plant("/home/s10/a.t", "one\ntwo\n");
    plant("/home/s10/b.t", "three\n");
    plant("/home/s10/skip.t", "x\n");

    // A function, an EXIT trap, a glob walked by `for`, a `case` that skips an
    // arm, a `$( )` with a redirection inside it, a here-doc and a status —
    // one file, and the first place all of them meet. It is also the example
    // Programming_Manual.md prints, so it has to work as written.
    plant("/home/s10/report.sh",
          'show() {\n' +
          '  echo "$1: $(wc < $1)"\n' +
          '}\n' +
          "trap 'echo bye' 0\n" +
          "for f in /home/s10/*.t\n" +
          "do\n" +
          "  case $f in\n" +
          "  *skip*) continue ;;\n" +
          "  esac\n" +
          "  show $f\n" +
          "done\n" +
          "cat <<EOF\n" +
          "end\n" +
          "EOF\n" +
          "exit 3\n");

    t.is("sh /home/s10/report.sh",
           "/home/s10/a.t: 2 2 8|/home/s10/b.t: 1 1 6|end|bye");

    // …and its status reaches the prompt through the whole of that.
    s = submit("clear", t.at(0.01));
    s = submit("sh /home/s10/report.sh", t.at(0.01));
    if (!rows(s).includes(prompt(3)))
        fail(`the script's exit 3 left ${row(s, s.cursor_y)}, expected ${prompt(3)}`);

    // The seams no per-stage block reaches. A function body that globs, with
    // the *call* redirected: the walk, the loop's Flow and Ctx::base at once.
    submit("g() { for f in /home/s10/*.t; do echo $f; done; }", t.at(0.01));
    ifile("g > /home/s10/out", "/home/s10/out",
          "/home/s10/a.t|/home/s10/b.t|/home/s10/skip.t");

    // `case` inside `while` inside a function, with the status carried out.
    submit("h() { while :; do case a in a) return 7;; esac; done; }", t.at(0.01));
    t.is("h; echo $?", "7");

    // A `for` over a substitution with `set -e` armed, in a subshell so this
    // shell survives it.
    t.is("(set -e; for i in $(echo a b); do echo $i; done)", "a|b");

    // A builtin at each end of a pipeline with a program between: `read` fills
    // this shell's own variable, so a stage is not a subshell.
    t.is("echo ax | grep a | read v; echo $v", "ax");

    // `set -x` traces the simple commands inside a compound, not the compound.
    t.is("(set -x; for i in a; do echo $i; done)", "+ echo a|a");

    // A construct through `-c`, which sends the whole string to the parser.
    t.is("sh -c 'for i in a b; do echo $i; done'", "a|b");

    // What the script costs: a program at a time and not one per turn of the
    // loop. Three instances at the peak — this shell, the script's own, and
    // whichever of `wc` and `cat` is running — however many turns it takes.
    submit("clear", t.at(0.01));
    net.peak = 0;
    submit("sh /home/s10/report.sh", t.at(0.01));
    if (net.peak !== 3)
        fail(`the script had ${net.peak} instances alive at once, expected 3`);

    submit("rm -r /home/s10", t.at(0.01));

    // exit ends the shell, and nothing runs after it — not even the rest of
    // its own line, which is Flow::Exit end to end. Last, for that reason.
    s = submit("clear", 14497);
    s = submit("echo before; exit 7; echo never", 14498);
    if (!rows(s).includes("before"))
        fail(`the list before exit printed ${JSON.stringify(rows(s))}`);
    if (rows(s).includes("never"))
        fail("a command ran after exit on the same line");
    if (!rows(s).some((line) => line.startsWith("the shell exited")))
        fail(`exit said nothing: ${JSON.stringify(rows(s))}`);
    s = submit("echo after", 14499);
    if (rows(s).includes("after"))
        fail("a command ran after the shell exited");
}

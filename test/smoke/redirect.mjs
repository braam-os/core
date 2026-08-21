// Here-documents, `>&`, `exec`, and the base stdio under them.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { fail, output, screen, shows, submit } from "./harness.mjs";

export function check() {
    let s = screen();
    const t = shows(13500, 0.005);
    // Redirection completion: here-documents, `>&`, `exec` and the base stdio
    // a function body and a compound now inherit.
    submit("mkdir /home/rd", t.at(0.01));

    // A here-doc is several typed lines: the shell accumulates until the
    // delimiter, which is what line_incomplete's `more` drives.
    const rlines = (lines, want) => {
        submit("clear", t.at());
        let s;
        for (const l of lines)
            s = submit(l, t.at());
        // The PS2 lines are echoed as they are typed; what the command
        // printed is what is left.
        const got = output(s).filter((l) => !l.startsWith("> ")).join("|");
        if (got !== want)
            fail(`${JSON.stringify(lines)} printed ${JSON.stringify(got)}, ` +
                 `expected ${JSON.stringify(want)}`);
    };

    rlines(["cat <<EOF", "hi", "EOF"], "hi");
    rlines(["cat <<EOF", "a", "b", "EOF"], "a|b");
    rlines(["v=x; cat <<EOF", "$v", "EOF"], "x");
    rlines(["cat <<'EOF'", "$v", "EOF"], "$v");
    rlines(["cat <<EOF | wc", "hi", "EOF"], "1 1 3");
    t.is("f() { echo b; }; f | wc", "1 1 2");
    // Two steps, because a line longer than the grid wraps and output() would
    // pick the wrapped tail up as a row of its own.
    const rfile = (line, path, want) => {
        submit(line, t.at());
        submit("clear", t.at());
        const got = output(submit("cat " + path, t.at())).join("|");
        if (got !== want)
            fail(`\`${line}\` left ${JSON.stringify(got)}, expected ${JSON.stringify(want)}`);
    };

    rfile("f() { echo r; }; f > /home/rd/a", "/home/rd/a", "r");

    // `2>&1` merges, and a real descriptor needs Sys::Dup to be in two slots.
    t.is("ls /nope 2>&1 | wc", "1 4 21");
    rfile("ls /nope > /home/rd/b 2>&1", "/home/rd/b", "ls: /nope: not found");

    // A compound may be redirected now, though not yet piped.
    rfile("{ echo a; echo b; } > /home/rd/c", "/home/rd/c", "a|b");
    rfile("for i in p q; do echo $i; done > /home/rd/d", "/home/rd/d", "p|q");
    t.is("{ echo x; } | wc", "syntax error: a compound command cannot be piped yet");

    // `( … )` runs here and puts back what it moved.
    t.is("(cd /bin; pwd); pwd", "/bin|/home");
    t.is("s=out; (s=in; echo $s); echo $s", "in|out");
    t.is("set -- z; (set -- a b; echo $#); echo $#", "2|1");
    t.is("(q() { echo n; }); q", "q: not found");
    // A redefinition inside one is put back too, not just a new name.
    t.is("p() { echo old; }; (p() { echo new; }); p", "old");
    t.is("(exit 3); echo $?", "3");
    rfile("(echo sub) > /home/rd/e", "/home/rd/e", "sub");

    // `exec` keeps its redirections, and they outlive the line — inside a
    // subshell, which is the only way back: there is no /dev/tty and no way
    // to name the stream this shell was handed.
    rfile("(exec > /home/rd/log; echo one; echo two)", "/home/rd/log", "one|two");
    t.is("(exec > /home/rd/log); echo back", "back");
    submit("rm -r /home/rd", t.at(0.01));
}

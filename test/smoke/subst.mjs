// Command substitution, functions, `.`, `eval` and `return`.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { shows, submit } from "./harness.mjs";

export function check() {
    const t = shows(13400, 0.005);
    // Command substitution: a pipe the shell drains itself. The unit suite has
    // the hook against a canned callback; what it cannot reach is a real
    // command writing down a real pipe.
    submit("mkdir /home/c", t.at(0.01));
    // Three copies of a 7,413-byte file: more than the eight writes a pipe
    // holds, so the drain has to be running before the wait or this hangs.
    submit("cat /etc/help /etc/help /etc/help > /home/c/big", t.at(0.01));

    t.is("echo $(echo hi)", "hi");
    t.is("echo `echo tick`", "tick"); // the backtick form
    t.is("echo a$(echo b)c", "abc");
    t.is("echo $(echo a b)", "a b"); // splits, then echo rejoins
    // A `$(…)` inside quotes quotes independently, and the captured spacing
    // survives because the field is not split.
    t.is("echo \"$(echo 'a   b')\"", "a   b");
    t.is("echo $(echo 'a   b')", "a b"); // unquoted: split, then rejoined
    t.is("echo '$(echo no)'", "$(echo no)"); // single quotes: as typed
    t.is("echo $(echo one; echo two)", "one two"); // a list, in order
    t.is("echo $(echo $(echo deep))", "deep"); // nested
    t.is("x=$(ls /bin | grep less); echo $x", "less"); // through a real pipeline
    t.is("echo $(echo hi | wc)", "1 1 3");
    t.is("echo $(pwd)/x", "/home/x"); // no trailing newline in the value
    t.is("echo $(jobs)done", "done"); // a builtin down the same pipe
    t.is("echo $(nosuchcmd) after", "nosuchcmd: not found|after");
    t.is("for f in $(echo p q); do echo $f; done", "p|q");
    t.is("case $(echo hi) in h*) echo yes;; esac", "yes");
    // The many-writes case: 22,239 bytes is forty chunks against eight
    // slots, so without drain-before-wait this one hangs rather than fails.
    // The counts are three copies of /etc/help, so a line added there moves
    // them.
    t.is("x=$(cat /home/c/big); echo \"$x\" | wc", "450 3453 22239");
    submit("rm -r /home/c", t.at(0.01));

    // Functions, `.`, `eval` and `return`. The unit suite has the grammar;
    // what it cannot see is a body that outlives its line, the positional
    // parameters going back, and a status carried out by `return`.
    submit("mkdir /home/fn", t.at(0.01));
    submit("echo 'g() { echo sourced $1; }' > /home/fn/lib.sh", t.at(0.01));

    t.is("f() { echo $1; }; f hi", "hi");
    t.is("f() { echo $#; }; f a b c", "3");
    t.is("f() { echo $1; }; f one; f two", "one|two");
    t.is("f() { echo in $1; }; set outer; f inner; echo $1", "in inner|outer");
    t.is("g() { echo one; }; g() { echo two; }; g", "two"); // redefinition
    // Ahead of a builtin and ahead of /bin. Both are unset again on the same
    // line: a definition outlives its line, which is the point, and one left
    // standing would shadow the name for every case after this one.
    t.is("cd() { echo shadowed; }; cd /tmp; unset -f cd", "shadowed");
    t.is("echo() { true; }; echo hi; unset -f echo; echo back", "back");
    t.is("f() { return 3; echo never; }; f; echo $?", "3");
    t.is("f() { echo a; return 3; echo b; }; f", "a");
    t.is("f() { while true; do return 7; done; }; f; echo $?", "7");
    t.is("x=1; f() { x=2; }; f; echo $x", "2"); // not a subshell
    t.is("f() { echo hi; }; unset -f f; f", "f: not found");
    t.is("f() { echo hi; }; f | wc", "1 1 3"); // S7 lifted the refusal
    t.is("f() { echo body; }; x=$(f); echo $x", "body"); // through a capture
    t.is("f() { echo $1; }; for i in p q; do f $i; done", "p|q");

    t.is("eval 'echo from eval'", "from eval");
    t.is("eval echo a b", "a b");
    t.is("eval 'x=set'; echo $x", "set"); // eval runs in this shell
    t.is("eval", "");
    t.is("return 4; echo still here", "still here"); // outside: a no-op

    t.is(". /home/fn/lib.sh; g there", "sourced there");
    t.is(". /home/fn/nosuch", "/home/fn/nosuch: not found");
    submit("rm -r /home/fn", t.at(0.01));
}

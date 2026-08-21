// S8: the scripting builtins, `read`, `wait`, `trap` and the options.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { CTRL, KEY, fail, net, press, rows, run, screen, shows, submit, type } from "./harness.mjs";

export function check() {
    let s = screen();
    const t = shows(13600, 0.005);
    // S8. The scripting builtins: `test` and `[`, `:`, `echo`, `true`, `false`,
    // `read`, `wait`, `trap` and the three option letters. The unit suite has
    // the expression grammar against a table; what only a real shell shows is
    // the grammar over a real store, and the options reaching the walk.
    submit("mkdir /home/s8", t.at(0.01));

    t.is("if [ -f /etc/motd ]; then echo yes; fi", "yes");
    t.is("if [ -d /bin ]; then echo yes; fi", "yes");
    t.is("[ -f /bin ]; echo $?", "1");
    t.is("test -d /bin; echo $?", "0");
    t.is("[ -f /etc/motd -a -d /bin ]; echo $?", "0");
    t.is("[ nope = nope ]; echo $?", "0");
    t.is("[ 2 -gt 1 ]; echo $?", "0");
    // Two that answer out of the gaps rather than out of a mode word: `-x` is
    // a file the kernel would instantiate, and `-w` is a writable mount.
    t.is("[ -x /bin/true ]; echo $?", "0");
    t.is("[ -x /etc/motd ]; echo $?", "1");
    t.is("[ -w /etc/motd ]; echo $?", "0");
    t.is("[ -w /proc/uptime ]; echo $?", "1");
    // The binary pair, over two files made in a known order.
    submit("touch /home/s8/a", t.at(0.01));
    submit("touch /home/s8/b", t.at(0.01));
    t.is("[ /home/s8/b -nt /home/s8/a ]; echo $?", "0");
    t.is("[ /home/s8/a -nt /home/s8/b ]; echo $?", "1");
    t.is("[ /home/s8/a -ot /home/s8/b ]; echo $?", "0");
    // And `touch` turning the answer round, which is what a `make` rides on.
    submit("touch /home/s8/a", t.at(0.01));
    t.is("[ /home/s8/a -nt /home/s8/b ]; echo $?", "0");
    // A missing file is false either way round, as is a directory, which keeps
    // no mtime at all.
    t.is("[ /home/s8/a -nt /home/s8/gone ]; echo $?", "1");
    t.is("[ /home/s8/gone -ot /home/s8/a ]; echo $?", "1");
    t.is("[ /home/s8/a -nt /home/s8 ]; echo $?", "0");
    t.is("[ /home/s8 -nt /home/s8/a ]; echo $?", "1");
    // Neither true nor false, and said so.
    t.is("[ -f /bin; echo $?", "test: ] missing|2");
    t.is("[ -f ]; echo $?", "test: argument expected|2");
    t.is("test -f; echo $?", "test: argument expected|2");
    t.is("test; echo $?", "1");
    // The same expression through the file, which is what a future find would
    // run: a builtin shadows the name at a prompt, not everywhere.
    t.is("/bin/test -d /bin; echo $?", "0");

    t.is(":; echo $?", "0");
    t.is("true; echo $?", "0");
    t.is("false; echo $?", "1");
    t.is("echo -n a; echo b", "ab");

    // `while [ … ]` spends no worker per turn, which is the whole of why the
    // rule grew a second clause.
    submit("clear", t.at(0.01));
    const links0 = net.links.length;
    submit("for i in 1 2 3 4 5; do [ -f /etc/motd ]; done", t.at(0.01));
    if (net.links.length !== links0)
        fail(`a loop of builtins hired ${net.links.length - links0} workers`);

    // `read` fills this shell's own variables, so a pipeline stage is not a
    // subshell and what it set is still there on the next command.
    t.is("echo a b c | read x y; echo $y", "b c");
    t.is("echo one | read v; echo $v", "one");
    submit("echo one > /home/s8/f", t.at(0.01));
    submit("echo two >> /home/s8/f", t.at(0.01));
    submit("echo three >> /home/s8/f", t.at(0.01));
    // Three lines out of one chunk: Sys::Read carries no length, so this is
    // the pushback buffer or it is nothing.
    t.is("while read l; do echo got $l; done < /home/s8/f", "got one|got two|got three");
    t.is("read a b < /home/s8/f; echo $a", "one");

    // The options, inside `( … )` so the shell survives them and so the
    // checkpoint is shown to put them back.
    t.is("(set -e; false; echo never); echo on", "on");
    t.is("(set -e; if false; then echo x; fi; echo cond)", "cond");
    t.is("(set -e; false || true; echo or)", "or");
    t.is("(set -e; ! false; echo not)", "not");
    t.is("(set -e; while false; do echo x; done; echo loop)", "loop");
    t.is("(set -eu; echo $-)", "eu");
    t.is("(set -e); echo -$-", "-");
    t.is("(set -u; echo $nosuch); echo on", "nosuch: parameter not set|on");
    t.is("(set -u; echo ${nosuch-ok})", "ok"); // the operators ask first
    t.is("(set -x; echo hi)", "+ echo hi|hi");
    t.is("(set -x; x=1 echo hi)", "+ x=1 echo hi|hi");
    t.is("set -q", "usage: set [-eux] [+eux] [--] [<arg>...]");
    // `set -x` is not `set --`: the parameters are the operands' to change.
    t.is("set a b; (set -x; echo $#); set --", "+ echo 2|2");

    // A whole script, where `set -e` means what it says: the shell stops.
    submit("echo 'set -e' > /home/s8/e.sh", t.at(0.01));
    submit("echo false >> /home/s8/e.sh", t.at(0.01));
    submit("echo 'echo never' >> /home/s8/e.sh", t.at(0.01));
    t.is("sh -s < /home/s8/e.sh; echo done", "done");

    // `trap … 0` fires on the way out, through a nested shell because this
    // one's exit would end the session.
    t.is("echo \"trap 'echo bye' 0\" | sh -s", "bye");
    t.is("trap 'echo x' 0; trap; trap - 0", "trap -- 'echo x' 0");
    // The one it cannot honour, and the ones that do not exist.
    t.is("trap '' 2", "trap: cannot ignore an interrupt");
    t.is("trap 'echo x' 9", "trap: 9: unsupported");

    // `wait` collects a background job without putting a prompt in between.
    s = submit("clear", t.at(0.01));
    s = submit("sleep -m 5000 & wait; echo waited", t.at(0.01));
    if (rows(s).includes("waited"))
        fail("wait came back before the job did");
    run(20000);
    s = screen();
    if (!rows(s).includes("waited"))
        fail(`wait never came back: ${JSON.stringify(rows(s))}`);
    // `trap … 2` fires where a ^C becomes a status, which is the only place it
    // can: the interrupt went to the stage, so this shell was never cancelled.
    submit("clear", t.at(0.01));
    submit("trap 'echo caught' 2", t.at(0.01));
    type("sleep -m 5000");
    press(KEY.ENTER);
    run(t.at(0.01));
    press("c".codePointAt(0), CTRL);
    run(t.at(0.01));
    s = screen();
    if (!rows(s).includes("caught"))
        fail(`a ^C with a trap on 2 printed ${JSON.stringify(rows(s))}`);
    submit("trap - 2", t.at(0.01));

    t.is("wait %9", "wait: no such job");
    t.is("wait; echo $?", "0"); // nothing to wait for

    submit("rm -r /home/s8", t.at(0.01));
}

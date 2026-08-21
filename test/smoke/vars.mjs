// Variables, the environment across a spawn, PATH, and what `command -v` says.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { fail, hasRootfs, prompt, rows, screen, shows, store } from "./harness.mjs";

export function check() {
    let s = screen();
    const { line: vrun, has: vshows } = shows(1163, 0.01);
    // Variables. The unit suite has the expander; what only a real shell shows
    // is that a value survives to the next line and that the fields reach argv.

    vrun("x=one");
    vshows("echo $x", "one");
    vshows("echo ${x}s", "ones");
    vshows("echo ${nosuch-alt}", "alt");
    vshows("echo ${nosuch?}", "nosuch: parameter not set");

    // Empty against absent, in two spaces: `"$x"` is an argument and `$x` is
    // not, which is the whole of the field flag.
    vrun("e=");
    vshows('echo a "$e" b', "a  b");
    vshows("echo a $e b", "a b");

    // Splitting against IFS, and quoting turning it off.
    vrun('two="a  b"');
    vshows("echo $two", "a b");
    vshows('echo "$two"', "a  b");

    // The positional parameters and $#.
    vrun("set p q r");
    vshows("echo $# $2", "3 q");
    vrun("shift");
    vshows("echo $# $*", "2 q r");

    // $? is the last command's — with no `clear` between, since that would be
    // the last command.
    vrun("clear");
    vrun("false");
    if (!rows(vrun("echo $?")).includes("1"))
        fail("$? did not carry the last command's status");

    // An assignment prefix does not stay. `echo` is a builtin, so the prefix is
    // applied around it and put back; a program gets it in its environment
    // instead, which is the block below.
    vrun("x=two echo hi");
    vshows("echo $x", "one");

    // Init's base environment reached the shell's table through _start, which
    // is what makes a variable nothing here set readable.
    vshows("echo $HOME", "/home");
    vshows("echo $SHELL", "/bin/sh");

    // An exported variable crosses a spawn; an unexported one does not.
    vrun("export ev=yes");
    vrun("unexp=no");
    vshows("env", "ev=yes");
    if (rows(vrun("env")).some((line) => line.startsWith("unexp=")))
        fail("an unexported variable reached a child");

    // An assignment prefix on a *program* goes into that child's environment
    // and nowhere else: the shell's own table is untouched.
    vshows("pfx=1 env", "pfx=1");
    vshows("echo [$pfx]", "[]");

    // The prefix stands in for an exported variable of the same name rather
    // than arriving beside it.
    vshows("ev=other env", "ev=other");
    if (rows(vrun("ev=other env")).includes("ev=yes"))
        fail("the exported value survived beside the prefix that replaced it");
    vshows("echo $ev", "yes");

    // `env` sets one for a command, and -i starts from nothing.
    vshows("env A=1 env", "A=1");
    vrun("clear");
    if (rows(vrun("env -i env")).some((line) => line.includes("=")))
        fail("env -i handed the child an environment");

    // A nested shell takes the environment into its own table, so an exported
    // variable survives a /bin/sh in between.
    vshows("sh -c 'echo $ev'", "yes");
    vshows("sh -c 'echo $HOME'", "/home");

    // A spawn that names no environment hands the child the caller's, which is
    // what a program that starts a program gets without doing anything.
    vshows("timeout -m 5000 env", "ev=yes");

    // PATH. Init plants one and the kernel searches it, so the /bin that used
    // to be a constant in exec_resolve is a value that a spawn carries.
    vshows("echo $PATH", "/bin:/pkg/bin");
    vrun("mkdir /home/pbin");
    vrun("ln -s /bin/echo /home/pbin/hi");
    if (!rows(vrun("hi one")).some((line) => line.startsWith("hi: not found")))
        fail("a program outside PATH ran by name");

    vrun("PATH=/home/pbin:/bin");
    vshows("hi one", "one");

    // An assignment marks it exported without being asked: an unexported PATH
    // would never reach the kernel that reads it.
    vshows("env", "PATH=/home/pbin:/bin");

    // The point of resolving kernel-side: a program that spawns searches it
    // too, and so does a nested shell.
    vshows("timeout -m 5000 hi two", "two");
    vshows("sh -c 'hi three'", "three");

    // A file that is not a program does not shadow the binary behind it —
    // there are no permissions, so being a program is the only test there is.
    vrun("echo text > /home/pbin/ls");
    vshows("ls /home", "pbin/");

    // ...and when it is all the search found, that is 126 rather than 127.
    s = vrun("PATH=/home/pbin ls");
    if (!rows(s).some((line) => line.startsWith("ls: not executable")))
        fail(`a search that found only a non-program: ${JSON.stringify(rows(s))}`);
    if (!rows(s).includes(prompt(126)))
        fail(`a search that found only a non-program did not report 126: ` +
             `${JSON.stringify(rows(s))}`);

    // A PATH that is there and empty names no directories; an absent one is the
    // kernel's default, which is how `env -i ls` still runs.
    if (!rows(vrun("PATH= ls")).some((line) => line.startsWith("ls: not found")))
        fail("an empty PATH still found a command");
    vshows("env -i ls /home", "pbin/");
    vrun("unset PATH");
    vshows("echo [$PATH]", "[]");
    vshows("ls /home", "pbin/");
    vrun("PATH=/home/pbin:/bin");

    // `command -v` says which of the three a word is, in the order they resolve.
    vshows("command -v ls", "/bin/ls");
    vshows("command -v cd", "cd");
    vshows("command -v hi", "/home/pbin/hi");
    vshows("command -v /bin/wc", "/bin/wc");
    vshows("f() { echo fn; }; command -v f", "f");
    s = vrun("command -v nosuch");
    if (!rows(s).includes(prompt(1)))
        fail(`command -v on a name that is nothing: ${JSON.stringify(rows(s))}`);
    s = vrun("command ls");
    if (!rows(s).includes(prompt(2)))
        fail(`command without -v: ${JSON.stringify(rows(s))}`);

    // A #! script is a program to all three, `help` being the one the archive
    // ships: `command -v` and `test -x` answer from the same probe the kernel
    // resolves with, and none of them wants a mode bit the store has nowhere
    // to keep.
    vshows("command -v help", "/bin/help");
    vshows("test -x /bin/help && echo runnable", "runnable");

    vrun("unset -f f");
    vrun("rm -r /home/pbin");
    vrun("PATH=/bin:/pkg/bin");

    // Activation. /pkg/bin is the second component of the default search list
    // and a symlink to the live generation, so an installed program is reached
    // the way every other program is. The tree here is what pkg's gen_ops
    // emits: absolute targets, /pkg/bin -> /pkg/active/bin -> /pkg/gen/N/bin.
    if (hasRootfs) {
        const echo = store.entries.find((e) => e.name === "bin/echo");
        if (!echo)
            fail("the archive carries no bin/echo");

        // Nothing installed: the component finds nothing, which is 127 and not
        // a failure of its own.
        s = vrun("hi one");
        if (!rows(s).some((line) => line.startsWith("hi: not found")))
            fail(`a missing /pkg was not an ordinary miss: ${JSON.stringify(rows(s))}`);
        if (!rows(s).includes(prompt(127)))
            fail(`a missing /pkg did not report 127: ${JSON.stringify(rows(s))}`);

        vrun("mkdir -p /pkg/store/hello-1.0-r0/bin");
        vrun("mkdir -p /pkg/gen/1/bin");
        store.files.set("/pkg/store/hello-1.0-r0/bin/hi", echo.bytes);
        vrun("ln -s /pkg/store/hello-1.0-r0/bin/hi /pkg/gen/1/bin/hi");
        vrun("ln -s /pkg/gen/1 /pkg/active");
        vrun("ln -s /pkg/active/bin /pkg/bin");

        // Reached by name through three links, with no PATH set by hand — and
        // by a program that spawns and a nested shell, since the search is the
        // kernel's.
        vshows("hi one", "one");
        vshows("timeout -m 5000 hi two", "two");
        vshows("sh -c 'hi three'", "three");
        vshows("command -v hi", "/pkg/bin/hi");

        // The kernel's default is what reaches it, not the shell's variable.
        vrun("unset PATH");
        vshows("hi four", "four");
        vrun("PATH=/bin:/pkg/bin");

        // /bin still wins for a name in both. wc rather than echo, which is a
        // builtin and would shadow the pair of them.
        vrun("ln -s /pkg/store/hello-1.0-r0/bin/hi /pkg/gen/1/bin/wc");
        vshows("command -v wc", "/bin/wc");
        vshows("echo one two | wc", "1 2 8"); // the real one; the copy would echo

        // PATH is a default and not a floor: a spawn that names one searches
        // that alone, installed programs included.
        s = vrun("PATH=/home hi five");
        if (!rows(s).includes(prompt(127)))
            fail(`a PATH of its own still found /pkg/bin: ${JSON.stringify(rows(s))}`);

        // A farm entry pointing at nothing, a dangling /pkg/active, and no
        // /pkg at all: one miss each, and no new failure path in the kernel.
        vrun("ln -s /pkg/store/gone-0/bin/x /pkg/gen/1/bin/ghost");
        if (!rows(vrun("ghost")).includes(prompt(127)))
            fail("a farm entry pointing at nothing was not 127");

        vrun("rm -r /pkg/gen/1");
        if (!rows(vrun("hi one")).includes(prompt(127)))
            fail("a dangling /pkg/active was not 127");

        vrun("rm -r /pkg");
        if (!rows(vrun("hi one")).includes(prompt(127)))
            fail("a /pkg that is gone again was not 127");
    }

    // A command name and a redirection target out of a variable: the argv
    // words split and the target does not.
    vrun("c=echo");
    vshows("$c via", "via");
    vrun("f=vfile");
    vrun("echo hi > $f");
    vshows("cat vfile", "hi");
    vrun("rm vfile");

    // readonly bites where export cannot.
    vrun("readonly r=keep");
    vshows("r=other", "r: cannot be set");
    vshows("echo $r", "keep");
    vrun("unset x");
    vshows("echo a $x b", "a b");

    // A name a `$` could not name is refused rather than marked, sets nothing
    // and reaches no child.
    vshows("export notes.txt", "export: notes.txt: not a valid name");
    vshows("export notes.txt; echo $?", "1");
    vshows("readonly a-b", "readonly: a-b: not a valid name");
    vshows("export 2a=1; echo $?", "1");
    if (rows(vrun("export 2a=1; env")).some((line) => line.startsWith("2a=")))
        fail("an illegal name reached a child's environment");
    // Refused before the read, so the line it would have eaten is still there.
    vshows("read notes.txt", "read: notes.txt: not a valid name");
    vshows("read notes.txt; echo $?", "1");
    vrun("echo kept > rf");
    vshows("{ read notes.txt; read ok; echo $ok; } < rf", "kept");
    vrun("rm rf");
}

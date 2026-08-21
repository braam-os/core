// S9: the entry points — a script by name, by `sh`, and by `#!`.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { fail, prompt, row, rows, screen, shows, store, submit } from "./harness.mjs";

export function check() {
    let s = screen();
    // S9: the entry points. The scripts are planted straight into the store,
    // so each sits next to the assertion and the archive does not grow.
    const t = shows(14200, 0.005);
    const plant = (path, text) => store.files.set(path, new TextEncoder().encode(text));

    submit("mkdir /home/s9", t.at(0.01));
    plant("/home/s9/loop.sh", "for i in 1 2 3\ndo\n  echo $i\ndone\n");
    plant("/home/s9/zero.sh", "echo $0\n");
    plant("/home/s9/pos.sh", "echo $1 $#\n");
    plant("/home/s9/seven.sh", "echo out\nexit 7\n");
    plant("/home/s9/err.sh", "false\necho never\n");
    plant("/home/s9/q.sh", "echo start\necho ${nosuch?is unset}\necho never\n");
    plant("/home/s9/bad.sh", "echo start\nif true\n");
    plant("/home/s9/hi.sh", "#!/bin/sh\necho hi $1 $#\n");
    plant("/home/s9/sp.sh", "#! /bin/sh\necho spaced\n");
    plant("/home/s9/tr.sh", "#!/bin/sh -x\necho traced\n");
    plant("/home/s9/who.sh", "#!/bin/sh\necho $0\n");
    plant("/home/s9/cat.sh", "#!/bin/cat\nthe file itself\n");
    plant("/home/s9/nest.sh", "#!/home/s9/hi.sh\necho never\n");
    plant("/home/s9/gone.sh", "#!/bin/nosuch\necho never\n");
    plant("/home/s9/plain.sh", "echo never\n");
    plant("/home/s9/deep.sh", "sh /home/s9/deep.sh\n");
    plant("/home/s9/x7.sh", "#!/bin/sh\necho out\nexit 7\n");
    plant("/home/s9/self.sh", "#!/bin/sh\nps\n");
    plant("/bin/greet", "#!/bin/sh\necho greetings\n");

    t.is("echo $0", "/bin/sh"); // argv[0], which for init's shell is the path
    t.is("sh /home/s9/loop.sh", "1|2|3");
    t.is("sh -s < /home/s9/loop.sh", "1|2|3"); // the older mode, still there
    t.is("sh /home/s9/zero.sh", "/home/s9/zero.sh");
    t.is("sh /home/s9/pos.sh x y", "x 2");
    t.is("sh -c 'echo hi'", "hi");
    // $0 is the word after the command string, which is v7's arithmetic.
    t.is("sh -c 'echo $0 $1' a b", "a b");
    t.is("sh -c 'echo $#' a b c", "2");
    t.is("sh -c 'exit 3'; echo $?", "3");
    t.is("sh -x -c 'echo hi'", "+ echo hi|hi");
    t.is("sh -e /home/s9/err.sh", "");
    t.is("sh /home/nosuch.sh; echo $?", "sh: /home/nosuch.sh: not found|127");
    t.is("sh -z", "usage: sh [-eux] [-s | -c <command> | <file>] [<arg>...]");
    t.is("sh -c", "usage: sh [-eux] [-s | -c <command> | <file>] [<arg>...]");
    // Parsed whole, as `.` is: a syntax error anywhere runs none of it.
    t.is("sh /home/s9/bad.sh; echo $?", "syntax error: expected 'then'|2");
    // ${x?} ends a script, which is what S9 decided and v7 does. The line it
    // is on is the second, so `start` printed and `never` did not.
    t.is("sh /home/s9/q.sh; echo $?", "start|nosuch: is unset|1");

    // A #! file is executable, and the interpreter is what instantiates.
    submit("cd /home/s9", t.at(0.01));
    t.is("./hi.sh a b", "hi a 2");
    t.is("/home/s9/hi.sh a b", "hi a 2"); // by absolute path, the same
    t.is("./sp.sh", "spaced");            // `#! /bin/sh`
    t.is("./tr.sh", "+ echo traced|traced");        // the interpreter's argument arrived
    t.is("./who.sh", "/home/s9/who.sh");            // $0 is the resolved script
    t.is("./cat.sh", "#!/bin/cat|the file itself"); // any interpreter, not just sh
    t.is("./hi.sh a b | wc", "1 3 7");              // a stage like any other
    t.is("sh ./hi.sh a b", "hi a 2");               // and the older way still works
    // A bare word finds /bin, and what the interpreter is handed is the
    // resolved path — a relative one would send it looking in its own cwd.
    t.is("greet", "greetings");

    t.is("test -x ./hi.sh; echo $?", "0");
    t.is("test -x ./plain.sh; echo $?", "1");
    t.is("test -x /bin/sh; echo $?", "0");

    // The refusals. A missing interpreter is 126 and not 127: the file is
    // there, and the interpreter is part of what makes it executable.
    t.is("./plain.sh; echo $?", "./plain.sh: not executable|126");
    t.is("./gone.sh; echo $?", "./gone.sh: not executable|126");
    t.is("./nest.sh; echo $?", "./nest.sh: not executable|126"); // one level only
    t.is("./nosuch.sh; echo $?", "./nosuch.sh: not found|127");
    // A chain of spawns stops at SYS_PROC_DEPTH; every level above the deepest
    // one returns its status, so the error is printed once.
    t.is("sh /home/s9/deep.sh; echo $?", "sh: too many processes|126");

    // /proc names the job by the word the caller typed, not by the interpreter
    // the kernel went on to instantiate, so ps, jobs and kill %n point at the
    // script and nothing here names /bin/sh at all.
    submit("clear", t.at(0.01));
    const psr = rows(submit("./self.sh", t.at(0.01)));
    if (!psr.some((line) => /^ *\d+ +\d+ \.\/self\.sh +[RS]\+ +\S+ +\d/.test(line)))
        fail(`ps inside a #! script did not name the script: ${JSON.stringify(psr)}`);
    if (psr.some((line) => /^ *\d+ +\d+ \/bin\/sh /.test(line)))
        fail(`ps inside a #! script named the interpreter: ${JSON.stringify(psr)}`);

    submit("cd /home", t.at(0.01));
    submit("rm /bin/greet", t.at(0.01));

    // A #! script's status reaches the prompt as any other program's does.
    s = submit("clear", t.at(0.01));
    s = submit("/home/s9/x7.sh", t.at(0.01));
    if (!rows(s).includes("out"))
        fail(`a #! script printed ${JSON.stringify(rows(s))}`);
    if (!rows(s).includes(prompt(7)))
        fail(`a #! script's exit 7 left ${row(s, s.cursor_y)}, expected ${prompt(7)}`);

    // A script's status reaches the parent's prompt as [n].
    s = submit("clear", t.at(0.01));
    s = submit("sh /home/s9/seven.sh", t.at(0.01));
    if (!rows(s).includes("out"))
        fail(`the script printed ${JSON.stringify(rows(s))}`);
    if (!rows(s).includes(prompt(7)))
        fail(`a script's exit 7 left ${row(s, s.cursor_y)}, expected ${prompt(7)}`);

    submit("rm -r /home/s9", t.at(0.01));
}

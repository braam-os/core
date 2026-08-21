// The store's directories and the listing's layout, plus mkdir -p.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import {
    chdir, fail, hasRootfs, output, prompt, row, rows, screen, submit, words,
} from "./harness.mjs";

export function check() {
    let s = screen();
    // /bin and /etc are directories in the store, unpacked at boot from the
    // archive tools/pack.py wrote at the end of the build.
    if (hasRootfs) {
        s = submit("clear", 1183);
        s = submit("cat /etc/motd", 1184);
        if (!rows(s).some((line) => line.startsWith("braam —")))
            fail(`/etc/motd did not read back: ${JSON.stringify(rows(s))}`);
        s = submit("clear", 1185);
        // Three names, and the grid is stdout, so they share a row; /etc/pkg
        // is the anchor's directory and is marked as one.
        s = submit("ls /etc", 1186);
        if (output(s).join("|") !== "help  motd  pkg/")
            fail(`/etc did not list its files: ${JSON.stringify(output(s))}`);
        // The README is at the root, where somebody arriving will see it. Not
        // the whole row: what else is at the top level moves with boot.cpp.
        s = submit("clear", 1186.1);
        s = submit("ls /", 1186.2);
        if (!words(s).includes("README"))
            fail(`/ did not list the README: ${JSON.stringify(output(s))}`);
    }

    // The layout itself, on a fixture the suite owns: /bin re-breaks whenever a
    // program is added. Sizes are chosen so name order, size order and reversed
    // order all differ. After vmstat, which counts the syscalls these make.
    submit("mkdir /home/t", 1186.30);
    submit("mkdir /home/t/sub", 1186.31);
    submit("echo 1 > /home/t/aaa", 1186.32); // 2 bytes
    submit("echo 123456 > /home/t/bb", 1186.33); // 7 bytes
    // Restamps aaa, so -t and -S disagree — and so `touch` on a file that is
    // already there is a moved mtime rather than a no-op.
    submit("touch /home/t/aaa", 1186.34);

    // aaa=3, bb=2, sub/=4, so the column is 4 + 2 wide and three of them fit.
    const listing = (line, now) => {
        submit("clear", now);
        return output(submit(line, now + 0.005)).join("|");
    };
    const listings = [
        ["ls /home/t", "aaa   bb    sub/"],
        ["ls -1 /home/t", "aaa|bb|sub/"],
        // A directory keeps no mtime in OPFS and reports a dash, not 1970.
        ["ls -l /home/t", "total 2|file 2 Jun 19 20:14 aaa|file 7 Jun 19 20:14 bb|" +
            "dir  0            - sub/"],
        ["ls -lh /home/t", "total 2|file 2B Jun 19 20:14 aaa|file 7B Jun 19 20:14 bb|" +
            "dir  0B            - sub/"],
        ["ls -S /home/t", "bb    aaa   sub/"],
        // Newest first, which the touch above made disagree with -S.
        ["ls -t /home/t", "aaa   bb    sub/"],
        ["ls -r /home/t", "sub/  bb    aaa"],
        ["ls -rS /home/t", "sub/  aaa   bb"],
        ["ls -rt /home/t", "sub/  bb    aaa"],
        // The last of -S and -t wins, as the last of -l, -1 and -C does.
        ["ls -St /home/t", "aaa   bb    sub/"],
        ["ls -tS /home/t", "bb    aaa   sub/"],
        ["ls -d /home/t", "/home/t/"],
        // Bundled, and a named directory gets no `total`.
        ["ls -dl /home/t", "dir  0            - /home/t/"],
        ["ls -- /home/t", "aaa   bb    sub/"],
        ["ls -R /home/t", "/home/t:|aaa   bb    sub/||/home/t/sub:"],
        ["ls -lR /home/t",
            "/home/t:|total 2|file 2 Jun 19 20:14 aaa|file 7 Jun 19 20:14 bb|" +
            "dir  0            - sub/||/home/t/sub:|total 0"],
        // A file operand prints before a directory one, in a block of its own.
        ["ls /home/t/aaa /home/t", "/home/t/aaa||/home/t:|aaa   bb    sub/"],
        // A pipe is one name per line; -C forces columns into one, at eighty.
        ["ls /home/t | head -n 2", "aaa|bb"],
        ["ls -C /home/t | cat", "aaa   bb    sub/"],
    ];
    let at = 1186.4;
    for (const [line, want] of listings) {
        const got = listing(line, at);
        at += 0.02;
        if (got !== want)
            fail(`\`${line}\` printed ${JSON.stringify(got)}, expected ${JSON.stringify(want)}`);
    }

    // A name is as wide as its codepoints, not its bytes: the grid puts one
    // codepoint in one cell, so padding by bytes shifts every column after an
    // accented name left by one. abcdef is six cells and six bytes, naïve five
    // cells and six, which is what tells the two apart.
    submit("mkdir /home/u", 1186.75);
    submit("touch /home/u/abcdef /home/u/naïve /home/u/xy", 1186.76);
    submit("clear", 1186.77);
    const wide = output(submit("ls /home/u", 1186.78)).join("|");
    if (wide !== "abcdef  naïve   xy")
        fail(`a UTF-8 name misaligned the columns: ${JSON.stringify(wide)}`);
    submit("rm -r /home/u", 1186.79);

    s = submit("clear", 1186.8);
    s = submit("ls -z /home/t", 1186.81);
    if (!output(s).some((line) => line.startsWith("ls: illegal option -- z")))
        fail(`an unknown flag said nothing: ${JSON.stringify(output(s))}`);
    if (!rows(s).includes(prompt(2)))
        fail(`an unknown flag left ${row(s, s.cursor_y)}, expected ${prompt(2)}`);

    submit("rm -r /home/t", 1186.9);

    // mkdir -p: the walk over the components, which is make_dir_all in
    // src/proc/io.cpp. Sys::MkDir is one level, so what is checked here is that
    // an existing directory is tolerated and a file in the way is not.
    {
        let mt = 1187;
        const mkdir = (line) => {
            submit("clear", (mt += 0.01));
            const s = submit(line, (mt += 0.01));
            return [output(s).join("|"), rows(s)];
        };
        const ok = (line) => {
            const [said, r] = mkdir(line);
            if (said !== "")
                fail(`\`${line}\` printed ${JSON.stringify(said)}, expected nothing`);
            if (!r.includes(prompt(0)))
                fail(`\`${line}\` left a status other than 0`);
        };
        const bad = (line, want, status = 1) => {
            const [said, r] = mkdir(line);
            if (!said.includes(want))
                fail(`\`${line}\` printed ${JSON.stringify(said)}, expected ${want}`);
            if (!r.includes(prompt(status)))
                fail(`\`${line}\` left a status other than ${status}`);
        };

        // One level at a time still refuses a missing parent.
        bad("mkdir /home/p/a/b", "not found");
        ok("mkdir -p /home/p/a/b");
        // Idempotent: every component of it stands now.
        ok("mkdir -p /home/p/a/b");
        // Without -p the leaf must not exist, which -p has not changed.
        bad("mkdir /home/p", "already exists");
        // A file where the leaf goes is an error even with -p, which is the
        // stat make_dir_all does when the whole path was already there.
        submit("touch /home/p/f", (mt += 0.01));
        bad("mkdir -p /home/p/f", "already exists");
        bad("mkdir -p /home/p/f/g", "mkdir: /home/p/f/g: ");
        // Relative, so the walk's prefixes resolve against the cwd.
        submit("cd /home/p", (mt += 0.01));
        chdir("/home/p");
        ok("mkdir -p r/s/t");
        const [seen] = mkdir("ls -d r/s/t");
        if (seen !== "r/s/t/")
            fail(`a relative mkdir -p left ${JSON.stringify(seen)}`);
        submit("cd /home", (mt += 0.01));
        chdir("/home");
        bad("mkdir -z /home/p", "mkdir: illegal option -- z", 2);
        submit("rm -r /home/p", (mt += 0.01));
    }
}

// /bin/unzip: the package manager's zip reader over an archive that need not
// be a package.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { fail, prompt, row, rows, shows, store, submit, zipOf } from "./harness.mjs";
import { archive } from "./pkgfix.mjs";

const { at, is, line } = shows(13700);

export function check() {
    // Stored entries, since §5.2 takes method 0 and this needs no compressor.
    store.files.set("/home/a.zip", zipOf([
        ["one", "first\n"],
        ["sub/two", "second\n"],
        ["sub/d/three", "third\n"],
    ]));

    // -l is the sizes right-aligned and then the names, in the archive's own
    // order — the central directory's, which zip.cpp keeps.
    is("unzip -l /home/a.zip", ["6  one", "7  sub/two", "6  sub/d/three"].join("|"));

    // -p writes an entry out and makes nothing.
    is("unzip -p /home/a.zip sub/two", "second");
    if (store.dirs.has("/home/sub"))
        fail("-p made a directory");

    // The default: every entry under the working directory, the parents made
    // on the way. §5.2 refuses an absolute or climbing name before it reaches
    // here, so there is no path of unzip's own to check.
    line("mkdir /home/z");
    is("cd /home/z; unzip /home/a.zip; cat one sub/two", "first|second");
    is("cat sub/d/three", "third");

    // -d puts the same tree somewhere else, making the destination too.
    is("cd /home; unzip -d /home/w /home/a.zip", "");
    is("cat /home/w/sub/d/three", "third");

    // A name filters, and one the archive has not got is reported without
    // stopping the rest.
    line("rm -r /home/w");
    is("unzip -d /home/w /home/a.zip one nope; ls /home/w",
       ["unzip: nope: not found", "one"].join("|"));

    // Method 8, which is Sys::Inflate; everything above is method 0.
    store.files.set("/home/d.zip", archive("hello-1.0-r0"));
    is("unzip -p /home/d.zip .PKGINFO | head -n 2", "P:hello|V:1.0-r0");

    // Reading the archive is a read like any other, and so is failing to.
    is("unzip /home/nothing.zip", "unzip: /home/nothing.zip: not found");
    store.files.set("/home/junk.zip", new TextEncoder().encode("not a zip"));
    is("unzip /home/junk.zip", "unzip: /home/junk.zip: invalid");

    // A usage error is 2, and -l with -p says nothing about which stdout is
    // for.
    submit("clear", at());
    let s = submit("unzip", at());
    if (!rows(s).includes("usage: unzip [-l] [-p] [-d <dir>] <archive> [<name>...]"))
        fail(`unzip printed ${JSON.stringify(rows(s))}, expected a usage line`);
    if (!rows(s).includes(prompt(2)))
        fail(`unzip left ${row(s, s.cursor_y)}, expected ${prompt(2)}`);

    submit("clear", at());
    s = submit("unzip -l -p /home/a.zip", at());
    if (!rows(s).includes(prompt(2)))
        fail(`unzip -l -p left ${row(s, s.cursor_y)}, expected ${prompt(2)}`);

    submit("clear", at());
    s = submit("unzip -q /home/a.zip", at());
    if (!rows(s).includes("unzip: illegal option -- q"))
        fail(`unzip -q printed ${JSON.stringify(rows(s))}`);

    submit("clear", at());
    s = submit("unzip -d", at());
    if (!rows(s).includes("unzip: option requires an argument -- d"))
        fail(`unzip -d printed ${JSON.stringify(rows(s))}`);

    line("cd /home; rm -r /home/z /home/w");
    line("rm /home/a.zip /home/junk.zip /home/d.zip");
    // The cases after this one start where the ones before it left the shell.
    is("pwd", "/home");
}

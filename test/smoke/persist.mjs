// The store across a reload: the stamp, /tmp, a foreign image and a reset.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import {
    fail, hasRootfs, instantiate, kernel, logged, names, press, prompt, regrid, resize, row, rows,
    run, screen, store, submit,
} from "./harness.mjs";

export function check() {
    let s = screen();
    // M5, first criterion, second half: throw the instance away and build a
    // new one against the same store. That is what a reload does, and the file
    // written above has to still be there afterwards.
    const before = logged.length;
    store.reopen();
    instantiate();
    kernel().init(0);
    if (logged.length !== before + 1)
        fail("the second boot did not log its banner");

    regrid(60, 16, "the reloaded kernel has no screen");
    run(2000);
    submit("clear", 2005); // the second boot banner is still on the grid
    s = submit("cat notes", 2010);
    const survived = rows(s).filter((line) => line && !line.includes("$"));
    if (survived.join(",") !== "one,two")
        fail(`the file did not survive the reload: ${JSON.stringify(survived)}`);

    // The stamp still matches, so the archive is not fetched, let alone
    // written: the steady state costs no download at all.
    if (hasRootfs && store.unpacks !== 1)
        fail(`a reload on a current store unpacked again (${store.unpacks} in all)`);

    // /tmp is the exception to the store's persistence, and emptying it at
    // boot is the whole of what makes it temporary now.
    submit("echo scratch > /tmp/note", 2020);
    if (!store.files.has("/tmp/note"))
        fail("/tmp is not writable");
    store.reopen();
    instantiate();
    kernel().init(0);
    resize(60, 16);
    run(2100);
    if (store.files.has("/tmp/note"))
        fail("boot did not wipe /tmp");
    if (!store.files.has("/home/notes"))
        fail("wiping /tmp took /home with it");

    // A stored image from another kernel is the user's to keep or replace, and
    // declining leaves what is there — including the binaries the shell that
    // is about to run comes out of.
    if (hasRootfs) {
        store.files.set("/etc/version", new TextEncoder().encode("0.0.1-stale"));
        store.files.set("/bin/keepme", new Uint8Array(1));
        // What pkg installed is under /pkg, which the archive does not carry,
        // so a release replaces the system and leaves it standing.
        store.dirs.add("/pkg");
        store.files.set("/pkg/keepme", new Uint8Array(1));
        store.reopen();
        instantiate();
        kernel().init(0);
        resize(60, 16);
        run(2200);
        s = screen();
        if (!rows(s).some((line) => line.includes("0.0.1-stale")))
            fail(`a stale stamp went unmentioned: ${JSON.stringify(rows(s))}`);
        if (!rows(s).some((line) => line.includes("replace /bin and /etc?")))
            fail(`boot did not ask before overwriting: ${JSON.stringify(rows(s))}`);

        press("n".codePointAt(0));
        run(2210);
        if (store.unpacks !== 1)
            fail("a declined upgrade unpacked anyway");
        if (!store.files.has("/bin/keepme"))
            fail("a declined upgrade replaced /bin regardless");
        s = screen();
        if (row(s, s.cursor_y) !== prompt())
            fail(`declining left ${row(s, s.cursor_y)}, expected a prompt`);

        // And accepting replaces both directories: what the archive does not
        // carry goes, and what it never names is untouched.
        store.reopen();
        instantiate();
        kernel().init(0);
        resize(60, 16);
        run(2300);
        press("y".codePointAt(0));
        run(2310);
        if (store.unpacks !== 2)
            fail(`an accepted upgrade unpacked ${store.unpacks - 1} times, expected 2 in all`);
        if (store.files.has("/bin/keepme"))
            fail("the unpack left a binary the archive does not carry");
        if (!store.files.has("/home/notes"))
            fail("the unpack reached outside the directories the archive names");
        if (!store.files.has("/pkg/keepme"))
            fail("a version change took /pkg with it");
        s = screen();
        if (row(s, s.cursor_y) !== prompt())
            fail(`accepting left ${row(s, s.cursor_y)}, expected a prompt`);
    }

    // M5's third criterion, retired: there is no memory fallback any more, so
    // a browser with no OPFS is told it cannot run rather than given a store
    // that quietly loses everything at the next reload.
    const entries = store.entries;
    store.reset();
    store.entries = entries; // served beside kernel.wasm; a reload still finds it
    store.opfs = false;
    store.sync = false;
    instantiate();
    kernel().init(0);
    resize(60, 16);
    run(3000);
    s = screen();
    if (!rows(s).some((line) => line.startsWith("this browser has no OPFS")))
        fail(`booting without OPFS said nothing: ${JSON.stringify(rows(s))}`);
    if (store.unpacks !== 0)
        fail("a system with no store unpacked into it anyway");
    if (rows(s).some((line) => line.includes("$")))
        fail(`booting without OPFS reached a prompt: ${JSON.stringify(rows(s))}`);

    // Back to a working store for what follows: the reset above emptied it, so
    // this boot is a first boot again and unpacks without asking.
    store.opfs = true;
    store.sync = true;
    instantiate();
    kernel().init(0);
    resize(60, 16);
    run(3005);
    s = screen();
    if (row(s, s.cursor_y) !== prompt())
        fail(`the store came back but the shell did not: ${JSON.stringify(rows(s))}`);
}

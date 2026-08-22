// §6.1's derived names, and a tab that dies mid-transaction.
// Part of the smoke suite; test/run.mjs runs the cases in order, the eight
// pkg cases share test/smoke/pkgfix.mjs, and doc/Testing.md has the rules.

import { OP, linkTarget } from "../../web/fs.js";
import {
    fail, instantiate, kernel, names, net, regrid, run, screen, store, submit,
} from "./harness.mjs";
import {
    RURL, archive, at, bytes, good, plant, prints, restore, serve, text, update,
} from "./pkgfix.mjs";

export function check() {
    let s = screen();
    // P26. §6.1's names, derived by tools/mkindex.py out of each package's
    // bin/ — nothing in the fixture writes cmd:hi down. Its own /pkg, and
    // index 1 again.
    submit("rm -r /pkg", at());
    store.dirs.add("/pkg");
    plant("/etc/repositories", RURL + "\n");
    serve("libz-1.0-r0", good);
    serve("hello-1.0-r0", archive("hello-1.0-r0"));
    prints("pkg update", `${RURL}|index 1, 2 packages`);

    // The prefix is a name like any other, so a name nothing provides is
    // the same refusal a package name gets.
    prints("pkg install cmd:nosuch", "pkg: cmd:nosuch: not in the index", 1);

    // §6.1's version clause: cmd:hi=1.0-r0 is selectable, so this picks
    // hello, which names libz. Unversioned it would be `cmd:hi (virtual)`.
    prints("pkg install cmd:hi",
           ["Installing libz (1.0-r0)", "Installing hello (1.0-r0)",
            "generation 1, 2 packages"].join("|"));
    if (text("/pkg/world") !== "cmd:hi\n")
        fail(`/pkg/world holds ${JSON.stringify(text("/pkg/world"))}`);
    // §8.1 is written from the index stanza, so the derived name is
    // installed with the package and a solve against either set sees it.
    if (!text("/pkg/db/hello-1.0-r0").includes("p:cmd:hi=1.0-r0\n"))
        fail("the record carries no derived provide");
    // The name and the farm entry are the same set (§8.3).
    prints("hi", "hi from hello");

    // P27. A tab that dies between the store write and the rename that
    // commits it (§8.3). The request is neither performed nor answered,
    // and the kernel is thrown away before it ever could be. Its own /pkg,
    // since this one is left mid-transaction on purpose.
    submit("rm -r /pkg", at());
    store.dirs.add("/pkg");
    plant("/etc/repositories", RURL + "\n");
    prints("pkg update", `${RURL}|index 1, 2 packages`);

    store.stall = (op, path) => op === OP.RENAME && path === "/pkg/active.new";
    submit("pkg install hello", at());
    if (store.held.length !== 1)
        fail(`the commit stalled ${store.held.length} requests, expected one`);

    // Everything the transaction had done is on disk, and the one step
    // that makes any of it visible is not.
    if (!store.files.has("/pkg/store/hello-1.0-r0/bin/hi"))
        fail("the store was never written");
    if (!store.files.has("/pkg/db/hello-1.0-r0"))
        fail("the record was never written");
    if (linkTarget(bytes("/pkg/gen/1/bin/hi")) !== "/pkg/store/hello-1.0-r0/bin/hi")
        fail("the farm was never built");
    if (!store.files.has("/pkg/active.new"))
        fail("the commit link was never written");
    if (store.files.has("/pkg/active"))
        fail("the rename happened after all");

    // The tab dies. The kernel and every worker go, the store stays, and
    // the token is never woken.
    store.reopen();
    instantiate();
    kernel().init(0);
    regrid(60, 16, "the kernel after the crash has no screen");
    run(at());

    // Nothing names the generation, so nothing is installed and nothing of
    // it is on PATH: §7's "nothing is half-installed" as a fact.
    prints("pkg list -i", "");
    prints("hi", "hi: not found", 127);

    // And the transaction is simply done again — the store directory it
    // left is the one the record vouches for, and the leftover link is
    // written over rather than tripped over. The generation it half-built
    // is not reused: numbering runs past what is there, so the one that
    // was never named stays unnamed.
    prints("pkg install hello",
           ["Installing libz (1.0-r0)", "Installing hello (1.0-r0)",
            "generation 2, 2 packages"].join("|"));
    prints("hi", "hi from hello");
    if (store.files.has("/pkg/active.new"))
        fail("the commit after the crash left /pkg/active.new behind");
    if (linkTarget(bytes("/pkg/active")) !== "/pkg/gen/2")
        fail(`/pkg/active names ${linkTarget(bytes("/pkg/active"))} after the crash`);

    // The abandoned generation is kept, because `pkg clean` cannot tell it
    // from a superseded one and neither can anything else: it is the
    // highest below the active one, which is what a rollback swings back
    // to. The archives go, since the store they were unpacked into stands.
    prints("pkg clean", "2 archives, 0 packages, 0 generations");
    if (!store.dirs.has("/pkg/gen/1"))
        fail("pkg clean collected the generation a rollback swings back to");

    // And it really is one. The rename was the last step, so everything
    // above it had already been written: swinging the link back runs what
    // the tab died before committing (§8.3).
    submit("rm /pkg/active", at());
    submit("ln -s /pkg/gen/1 /pkg/active", at());
    prints("hi", "hi from hello");

    // Put the release's /etc back and leave no /pkg, so what follows starts
    // where it did.
    restore();
    net.routes.delete(RURL + "/index");
    net.routes.delete(`${RURL}/libz-1.0-r0.zip`);
    net.routes.delete(`${RURL}/hello-1.0-r0.zip`);
    submit("rm -r /pkg", at());
}

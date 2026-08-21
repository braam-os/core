// §5.1's install scripts and §11's triggers, and the order they run in.
// Part of the smoke suite; test/run.mjs runs the cases in order, the eight
// pkg cases share test/smoke/pkgfix.mjs, and doc/Testing.md has the rules.

import { linkTarget } from "../../web/fs.js";
import { fail, net, screen, store, submit, type } from "./harness.mjs";
import {
    RURL, archive, at, bytes, good, plant, prints, repo, serve, text, update,
} from "./pkgfix.mjs";

export function check() {
    let s = screen();
    // P24. §5.1's scripts, which `noisy` carries all six of and nothing
    // else in the repository carries any of. Its own indexes, so the
    // assertions above are not disturbed by a package that makes noise.
    plant("/pkg/repositories", RURL + "\n");
    net.routes.set(RURL + "/index",
                   { status: 200, headers: "content-type: text/plain\n",
                     body: repo("index3") });
    serve("noisy-1.0-r0", archive("noisy-1.0-r0"));
    serve("noisy-1.1-r0", archive("noisy-1.1-r0"));
    prints("pkg update", `${RURL}|index 3, 1 package`);

    // pre- before the rename, post- after it, and apk's argv: the new
    // version, with nothing after it on an install.
    prints("pkg install noisy",
           ["Installing noisy (1.0-r0)", "pre-install 1.0-r0",
            "post-install 1.0-r0", "generation 1, 1 package"].join("|"));
    prints("noisy", "noisy 1.0-r0");

    // The scripts are ordinary recorded files: kept in the store so a
    // deinstall still has them, listed, and hashed like any other.
    prints("pkg files noisy",
           ["/pkg/store/noisy-1.0-r0/.post-deinstall",
            "/pkg/store/noisy-1.0-r0/.post-install",
            "/pkg/store/noisy-1.0-r0/.post-upgrade",
            "/pkg/store/noisy-1.0-r0/.pre-deinstall",
            "/pkg/store/noisy-1.0-r0/.pre-install",
            "/pkg/store/noisy-1.0-r0/.pre-upgrade",
            "/pkg/store/noisy-1.0-r0/bin/noisy"].join("|"));
    prints("pkg verify", "");

    // An upgrade runs the other pair, and the old version after the new.
    // The post- script fails: §11 records that and does not abort, so the
    // generation is committed and the run still succeeds.
    net.routes.set(RURL + "/index",
                   { status: 200, headers: "content-type: text/plain\n",
                     body: repo("index4") });
    prints("pkg update", `${RURL}|index 4, 1 package`);
    prints("pkg upgrade",
           ["Upgrading noisy (1.0-r0 -> 1.1-r0)", "pre-upgrade 1.1-r0 1.0-r0",
            "post-upgrade 1.1-r0 1.0-r0",
            "pkg: noisy-1.1-r0: post-upgrade failed (1)",
            "generation 2, 1 package"].join("|"));
    if (linkTarget(bytes("/pkg/active")) !== "/pkg/gen/2")
        fail("a failing script stopped the commit");
    prints("noisy", "noisy 1.1-r0");

    // Which is what gives pkg verify something to find.
    if (!text("/pkg/db/noisy-1.1-r0").includes("b:post-upgrade\n"))
        fail(`the record does not say what broke: ${text("/pkg/db/noisy-1.1-r0")}`);
    prints("pkg verify", "broken    /pkg/db/noisy-1.1-r0", 1);
    prints("pkg verify noisy", "broken    /pkg/db/noisy-1.1-r0", 1);

    // A removal runs the deinstall pair, out of a store directory the
    // archive has not been near since it was installed.
    prints("pkg remove noisy",
           ["Purging noisy (1.1-r0)", "pre-deinstall 1.1-r0", "post-deinstall 1.1-r0",
            "generation 3, 0 packages"].join("|"));

    // A mark from the pre- pass, which the commit writes the record
    // between the making of and the writing of. 1.1's pre-install fails,
    // and a fresh install is the only thing that runs one.
    submit("rm -r /pkg/store /pkg/db", at());
    prints("pkg install noisy",
           ["Installing noisy (1.1-r0)", "pre-install 1.1-r0",
            "pkg: noisy-1.1-r0: pre-install failed (1)", "post-install 1.1-r0",
            "generation 4, 1 package"].join("|"));
    if (!text("/pkg/db/noisy-1.1-r0").includes("b:pre-install\n"))
        fail("the commit overwrote a mark the pre- pass made");
    prints("pkg verify", "broken    /pkg/db/noisy-1.1-r0", 1);

    // §11's last sentence: a package that only places files runs no code.
    net.routes.set(RURL + "/index",
                   { status: 200, headers: "content-type: text/plain\n",
                     body: repo("index") });
    submit("rm -r /pkg", at());
    store.dirs.add("/pkg");
    plant("/pkg/repositories", RURL + "\n");
    serve("libz-1.0-r0", good);
    serve("hello-1.0-r0", archive("hello-1.0-r0"));
    prints("pkg update", `${RURL}|index 1, 2 packages`);
    prints("pkg install hello",
           ["Installing libz (1.0-r0)", "Installing hello (1.0-r0)",
            "generation 1, 2 packages"].join("|"));
    prints("pkg verify", "");

    net.routes.delete(`${RURL}/noisy-1.0-r0.zip`);
    net.routes.delete(`${RURL}/noisy-1.1-r0.zip`);

    // P25. `hooky` watches every package's share/ and /pkg/bin, and echoes
    // what it is handed. Its own index again, so what is above is not
    // disturbed by a package that exists to watch.
    submit("rm -r /pkg", at());
    store.dirs.add("/pkg");
    plant("/pkg/repositories", RURL + "\n");
    net.routes.set(RURL + "/index",
                   { status: 200, headers: "content-type: text/plain\n",
                     body: repo("index5") });
    serve("hooky-1.0-r0", archive("hooky-1.0-r0"));
    serve("bad-1.0-r0", archive("bad-1.0-r0"));
    prints("pkg update", `${RURL}|index 5, 3 packages`);

    // Freshly installed, so it sees every directory its globs name and not
    // only what this transaction wrote — its own share/ among them.
    prints("pkg install hooky",
           ["Installing hooky (1.0-r0)", "trigger /pkg/bin /pkg/store/hooky-1.0-r0/share/hooky",
            "generation 1, 1 package"].join("|"));

    // Not fresh any more: a second package's share/ is what the
    // transaction wrote, and that alone is what it is handed.
    prints("pkg install libz",
           ["Installing libz (1.0-r0)", "trigger /pkg/bin /pkg/store/libz-1.0-r0/share/libz",
            "generation 2, 2 packages"].join("|"));

    // A removal writes nothing to the store, so /pkg/bin is the whole of
    // what changed — the clause that exists so a removal can be noticed.
    prints("pkg remove libz",
           ["Purging libz (1.0-r0)", "trigger /pkg/bin", "generation 3, 1 package"].join("|"));

    // A trigger is a script, so §11 records a failing one the same way.
    // Two fire here, in the changeset's order, and one failing does not
    // stop the other.
    prints("pkg install bad",
           ["Installing bad (1.0-r0)", "trigger /pkg/bin",
            "pkg: bad-1.0-r0: trigger failed (3)",
            "trigger /pkg/bin /pkg/store/bad-1.0-r0/share/bad",
            "generation 4, 2 packages"].join("|"));
    if (!text("/pkg/db/bad-1.0-r0").includes("b:trigger\n"))
        fail("a failing trigger was not recorded");
    prints("pkg verify", "broken    /pkg/db/bad-1.0-r0", 1);

    net.routes.delete(`${RURL}/hooky-1.0-r0.zip`);
    net.routes.delete(`${RURL}/bad-1.0-r0.zip`);
    net.routes.set(RURL + "/index",
                   { status: 200, headers: "content-type: text/plain\n",
                     body: repo("index") });
}

// pkg clean: the collector, and the two things it must not collect.
// Part of the smoke suite; test/run.mjs runs the cases in order, the eight
// pkg cases share test/smoke/pkgfix.mjs, and doc/Testing.md has the rules.

import { fail, names, net, screen, store, submit, type } from "./harness.mjs";
import { RURL, archive, at, good, plant, prints, repo, serve, text, update } from "./pkgfix.mjs";

export function check() {
    let s = screen();
    // P22. The collector, and the two things it must not collect. It needs
    // several generations to have anything to choose between, so the tree
    // is built up rather than planted.
    store.dirs.add("/pkg");
    plant("/pkg/repositories", RURL + "\n");
    net.routes.set(RURL + "/index",
                   { status: 200, headers: "content-type: text/plain\n", body: repo("index") });
    serve("libz-1.0-r0", good);
    serve("hello-1.0-r0", archive("hello-1.0-r0"));
    serve("hello-1.1-r0", archive("hello-1.1-r0"));
    prints("pkg update", `${RURL}|index 1, 2 packages`);
    prints("pkg install hello",
           ["Installing libz (1.0-r0)", "Installing hello (1.0-r0)",
            "generation 1, 2 packages"].join("|"));

    // One generation supersedes nothing, so the cache is the whole of it —
    // and what is installed still runs afterwards.
    prints("pkg clean", "2 archives, 0 packages, 0 generations");
    if (store.files.has("/pkg/cache/hello-1.0-r0.zip"))
        fail("pkg clean left the cache behind");
    prints("hi", "hi from hello");
    prints("pkg clean", "nothing to clean");

    // Three generations, and a version the store holds that no generation
    // names any more.
    net.routes.set(RURL + "/index",
                   { status: 200, headers: "content-type: text/plain\n",
                     body: repo("index2") });
    prints("pkg update", `${RURL}|index 2, 2 packages`);
    prints("pkg upgrade",
           ["Upgrading hello (1.0-r0 -> 1.1-r0)", "generation 2, 2 packages"].join("|"));
    plant("/pkg/world", "");
    prints("pkg autoremove",
           ["Purging hello (1.1-r0)", "Purging libz (1.0-r0)",
            "generation 3, 0 packages"].join("|"));

    prints("pkg clean",
           ["Dropping hello (1.0-r0)", "Dropping generation 1",
            "1 archive, 1 package, 1 generation"].join("|"));

    // The record goes with the bytes: left behind, its digest is what
    // stem_state compares before it looks for the directory, so a
    // republished package would be refused rather than fetched.
    if (store.dirs.has("/pkg/store/hello-1.0-r0") || store.files.has("/pkg/db/hello-1.0-r0"))
        fail("a collected store directory kept its record, or the other way round");
    // Generation 2 still names both, so both are still there.
    if (!store.dirs.has("/pkg/store/hello-1.1-r0") || !store.dirs.has("/pkg/store/libz-1.0-r0"))
        fail("pkg clean dropped what a kept generation names");
    if (store.dirs.has("/pkg/gen/1"))
        fail("pkg clean kept the superseded generation");
    if (!store.dirs.has("/pkg/gen/2") || !store.dirs.has("/pkg/gen/3"))
        fail("pkg clean took the active generation or the one before it");

    // Rolled back: the generation above the live one is where rolling
    // forward goes, and a clean that took it would make P18's property
    // one-way.
    submit("rm /pkg/active", at());
    submit("ln -s /pkg/gen/2 /pkg/active", at());
    prints("hi", "hi from hello 1.1");
    prints("pkg clean", "nothing to clean");
    if (!store.dirs.has("/pkg/gen/3"))
        fail("a clean after a rollback took the generation to roll forward to");

    // With no /pkg/active every generation is above the active one, which
    // is the same rule arriving at the safe answer rather than a case of
    // its own.
    submit("rm /pkg/active", at());
    prints("pkg clean", "nothing to clean");
    if (!store.dirs.has("/pkg/gen/2") || !store.dirs.has("/pkg/gen/3"))
        fail("a clean with no /pkg/active dropped a generation");
    submit("ln -s /pkg/gen/3 /pkg/active", at());

    // And what it leaves is a tree that installs again: the version it
    // collected is missing rather than remembered, so this refetches.
    plant("/pkg/index", repo("index"));
    prints("pkg install hello",
           ["Installing libz (1.0-r0)", "Installing hello (1.0-r0)",
            "generation 4, 2 packages"].join("|"));
    prints("hi", "hi from hello");

    prints("pkg clean please", "Usage: pkg clean", 2);

    // Nothing to clean builds no tree to say so, as pkg remove does not.
    submit("rm -r /pkg", at());
    prints("pkg clean", "nothing to clean");
    if (store.dirs.has("/pkg") || store.files.has("/pkg/bin"))
        fail("a clean with nothing to clean built /pkg");
    store.dirs.add("/pkg");
}

// pkg remove: what a removal drops, what it keeps, and what autoremove is for.
// Part of the smoke suite; test/run.mjs runs the cases in order, the eight
// pkg cases share test/smoke/pkgfix.mjs, and doc/Testing.md has the rules.

import { linkTarget } from "../../web/fs.js";
import { fail, names, net, screen, store, submit } from "./harness.mjs";
import { RURL, archive, at, bytes, good, plant, prints, serve, text, update } from "./pkgfix.mjs";

export function check() {
    let s = screen();
    // P19. The install suite left /pkg broken on purpose, so this starts
    // from a tree of its own.
    submit("rm -r /pkg", at());
    store.dirs.add("/pkg");
    plant("/pkg/repositories", RURL + "\n");
    serve("libz-1.0-r0", good);
    serve("hello-1.0-r0", archive("hello-1.0-r0"));
    prints("pkg update", `${RURL}|index 1, 2 packages`);
    prints("pkg install hello",
           ["Installing libz (1.0-r0)", "Installing hello (1.0-r0)",
            "generation 1, 2 packages"].join("|"));

    // A dependency goes with what wanted it: every re-solve drops what
    // world no longer reaches.
    prints("pkg remove hello",
           ["Purging hello (1.0-r0)", "Purging libz (1.0-r0)",
            "generation 2, 0 packages"].join("|"));
    if (text("/pkg/world") !== "")
        fail(`/pkg/world holds ${JSON.stringify(text("/pkg/world"))} after a removal`);
    if (text("/pkg/gen/2/packages") !== "")
        fail("the new generation is not empty");
    if (linkTarget(bytes("/pkg/active")) !== "/pkg/gen/2")
        fail(`/pkg/active names ${linkTarget(bytes("/pkg/active"))}`);
    prints("hi", "hi: not found", 127);

    // A removal drops no bytes: the previous generation still names them,
    // and collecting them is pkg clean's (P22).
    if (!store.dirs.has("/pkg/store/hello-1.0-r0") || !store.files.has("/pkg/db/hello-1.0-r0"))
        fail("a removal took the store or the record with it");
    if (!store.dirs.has("/pkg/gen/1"))
        fail("a removal took the generation to roll back to");

    // Back again — off the cache, since a removal never emptied it.
    net.routes.delete(`${RURL}/hello-1.0-r0.zip`);
    net.routes.delete(`${RURL}/libz-1.0-r0.zip`);
    prints("pkg install hello",
           ["Installing libz (1.0-r0)", "Installing hello (1.0-r0)",
            "generation 3, 2 packages"].join("|"));

    // SOLVE_REMOVE unseats a preference; it does not uninstall. What
    // stays is reported, not forced.
    prints("pkg remove libz",
           ["pkg: libz: still needed by hello", "generation 3, unchanged"].join("|"), 1);
    if (store.dirs.has("/pkg/gen/4"))
        fail("a removal that did not happen built a generation");

    // The same where the name was explicit: world loses it though the
    // package stays, and a package nobody wants any more must say so.
    plant("/pkg/world", "hello\nlibz\n");
    prints("pkg remove libz",
           ["pkg: libz: still needed by hello; world updated",
            "generation 3, unchanged"].join("|"), 1);
    if (text("/pkg/world") !== "hello\n")
        fail(`/pkg/world holds ${JSON.stringify(text("/pkg/world"))}`);

    // A name that is neither in world nor installed is what was asked for.
    prints("pkg remove nonesuch", "generation 3, unchanged");

    // Nothing is orphaned, so autoremove has nothing to drop.
    prints("pkg autoremove", "generation 3, unchanged");

    // World edited by hand: the case autoremove exists for.
    plant("/pkg/world", "");
    prints("pkg autoremove",
           ["Purging hello (1.0-r0)", "Purging libz (1.0-r0)",
            "generation 4, 0 packages"].join("|"));

    // A world entry nothing installed satisfies stops a removal: with no
    // repository there is nowhere else to look, and the label is the
    // solver's. pkg install is what repairs it.
    plant("/pkg/world", "ghost\n");
    prints("pkg autoremove",
           ["pkg: cannot remove:", "  ghost (no such package)"].join("|"), 1);
    plant("/pkg/world", "");

    // Two operands where the first already purges the second: no spurious
    // survivor, and still one generation.
    prints("pkg install hello",
           ["Installing libz (1.0-r0)", "Installing hello (1.0-r0)",
            "generation 5, 2 packages"].join("|"));
    prints("pkg remove hello libz",
           ["Purging hello (1.0-r0)", "Purging libz (1.0-r0)",
            "generation 6, 0 packages"].join("|"));

    // There is no generation 0, and a removal builds no tree to say so.
    submit("rm -r /pkg", at());
    prints("pkg remove nonesuch", "nothing installed");
    if (store.dirs.has("/pkg") || store.files.has("/pkg/bin"))
        fail("a removal with nothing to remove built /pkg");
    prints("pkg autoremove", "nothing installed");

    prints("pkg remove", "usage: pkg remove <package>...", 2);
    prints("pkg autoremove please", "usage: pkg autoremove", 2);
}

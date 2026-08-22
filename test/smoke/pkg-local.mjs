// pkg install over a path and a URL: Package_Management.md §7's second way
// bytes are named, and what a .PKGINFO nothing vouched for may claim.
// Part of the smoke suite; test/run.mjs runs the cases in order, the pkg cases
// share test/smoke/pkgfix.mjs, and doc/Testing.md has the rules.

import { linkTarget } from "../../web/fs.js";
import { fail, net, store, submit, zipOf } from "./harness.mjs";
import {
    RURL, archive, at, bytes, good, plant, prints, repo, serve, text,
} from "./pkgfix.mjs";

// A package no index lists, built here: mkpkg.py runs at build time and a
// sideload's whole point is an archive that arrived some other way. Stored
// entries, since §5.2 takes method 0 and nothing here needs a compressor.
const pkg = (info, files = [["bin/mine", "#!/bin/sh\necho mine\n"]]) =>
    zipOf([[".PKGINFO", info], ...files]);

// It depends on an index package, so installing it is also the proof that a
// stanza nothing vouched for is a first-class input to the solver.
const MINE = "P:mine\nV:1.0-r0\nT:carried in by hand\nD:libz\n";

const put = (path, body) => store.files.set(path, body);

export function check() {
    // Its own /pkg and index 1, the way pkg-install and pkg-crash take one:
    // the generation numbering below is then this case's alone.
    submit("rm -r /pkg", at());
    store.dirs.add("/pkg");
    plant("/etc/repositories", RURL + "\n");
    serve("libz-1.0-r0", good);
    serve("hello-1.0-r0", archive("hello-1.0-r0"));
    prints("pkg update", `${RURL}|index 1, 2 packages`);

    // ---------------------------------------------------------- a sideload

    put("/home/mine-1.0-r0.zip", pkg(MINE));

    // §7: what named these bytes is the person who typed the path, and pkg
    // says so before it acts. The .PKGINFO is the stanza — the name and the
    // version in the plan came out of the archive, not off the command line —
    // and its D: is solved like any other, so libz comes off the index and is
    // checked against it in the same transaction.
    prints("pkg install /home/mine-1.0-r0.zip",
           ["pkg: mine-1.0-r0: unverified: no index vouches for it",
            "Installing libz (1.0-r0)", "Installing mine (1.0-r0)",
            "generation 1, 2 packages"].join("|"));

    // §8.1: G is 0 when nothing vouched, and the record is otherwise a record
    // like any other — the file list is what `pkg verify` re-reads.
    const record = text("/pkg/db/mine-1.0-r0");
    for (const want of ["P:mine\n", "V:1.0-r0\n", "G:0\n", "F:bin\n", "R:mine\n", "Z:Q2"])
        if (!record.includes(want))
            fail(`/pkg/db/mine-1.0-r0 carries no ${JSON.stringify(want)}`);

    // World names the package, not the path it came from: a later solve asks
    // for `mine` and not for a file that may be gone.
    if (text("/pkg/world") !== "mine\n")
        fail(`/pkg/world holds ${JSON.stringify(text("/pkg/world"))}`);
    if (linkTarget(bytes("/pkg/gen/1/bin/mine")) !== "/pkg/store/mine-1.0-r0/bin/mine")
        fail(`the farm names ${linkTarget(bytes("/pkg/gen/1/bin/mine"))}`);
    prints("mine", "mine");

    // §6.1's names are derived from the archive's flat bin/ here, since no
    // index wrote them down — so `cmd:` resolves for a sideload too.
    if (!record.includes("p:cmd:mine=1.0-r0\n"))
        fail(`the record carries no derived cmd: name: ${JSON.stringify(record)}`);

    // Visible afterwards, both ways round: a sideload the index does not list
    // would be invisible to `info` without the fallback to its own record.
    prints("pkg info mine",
           ["name          mine", "version       1.0-r0", "installed     1.0-r0",
            "vouched       no", "size          " + store.files.get("/home/mine-1.0-r0.zip").length,
            "description   carried in by hand", "depends       libz",
            "provides      cmd:mine=1.0-r0",
            "digest        " + record.match(/^C:(\S+)$/m)[1]].join("|"));

    // `pkg verify` reports it and does not fail over it: nothing is wrong,
    // this is how the package arrived.
    prints("pkg verify", "unvouched /pkg/db/mine-1.0-r0");

    // And the solver keeps knowing it. /pkg/db is read back into the installed
    // set every run and an installed-only package stays selectable, so an
    // upgrade against an index that has never heard of this one leaves it
    // standing rather than solving it away.
    prints("pkg upgrade", "generation 1, unchanged");

    // Its D: is honoured from the record too, so what it needs is not taken
    // out from under it — the request drops world's preference and libz stays,
    // exactly as it would for a package the index vouched for.
    prints("pkg remove libz",
           ["pkg: libz: still needed by mine", "generation 1, unchanged"].join("|"), 1);

    // ------------------------------------------------------- a URL sideload

    net.routes.set("https://x.example/other-2.0-r0.zip",
                   { status: 200, headers: "content-type: application/zip\n",
                     body: pkg("P:other\nV:2.0-r0\n", [["share/other", "x"]]) });
    prints("pkg install https://x.example/other-2.0-r0.zip",
           ["pkg: other-2.0-r0: unverified: no index vouches for it",
            "Installing other (2.0-r0)", "generation 2, 3 packages"].join("|"));
    if (!text("/pkg/db/other-2.0-r0").includes("G:0\n"))
        fail("a URL sideload was recorded as vouched for");

    // ------------------------------------------- the same bytes, by hand

    // An archive the index does turn out to list is the repository's package:
    // the digest is what is asked, not how it arrived. Nothing serves it, so
    // this also proves the fetch was skipped.
    put("/home/hello.zip", archive("hello-1.0-r0"));
    net.routes.delete(`${RURL}/hello-1.0-r0.zip`);
    prints("pkg install /home/hello.zip",
           ["Installing hello (1.0-r0)", "generation 3, 4 packages"].join("|"));
    if (!text("/pkg/db/hello-1.0-r0").includes("G:1\n"))
        fail("an indexed archive carried by hand lost its G");
    serve("hello-1.0-r0", archive("hello-1.0-r0"));

    // ------------------------------------------------------- the refusals

    // The store path is built out of P and V, so neither may climb out of it.
    put("/home/bad.zip", pkg("P:../escape\nV:1\n"));
    prints("pkg install /home/bad.zip", "pkg: /home/bad.zip: pkginfo: invalid", 1);
    if (store.dirs.has("/pkg/gen/4"))
        fail("a refused sideload built a generation");

    // §5.1: .PKGINFO is required, and an unknown top-level dot-entry makes the
    // package uninstallable.
    put("/home/nometa.zip", zipOf([["bin/x", "x"]]));
    prints("pkg install /home/nometa.zip", "pkg: /home/nometa.zip: metadata: not found", 1);
    put("/home/odd.zip", zipOf([[".PKGINFO", MINE], [".surprise", "x"]]));
    prints("pkg install /home/odd.zip", "pkg: /home/odd.zip: metadata: unsupported", 1);

    // The index stays authoritative wherever it speaks: a well-formed package
    // calling itself a name-version the index lists, at bytes it never signed,
    // is refused rather than preferred. This is the one that matters — without
    // it a path could quietly stand in for a repository's package.
    put("/home/forged.zip", pkg("P:hello\nV:1.0-r0\n", [["bin/hi", "#!/bin/sh\necho no\n"]]));
    prints("pkg install /home/forged.zip", "pkg: /home/forged.zip: index: permission denied", 1);
    if (text("/pkg/store/hello-1.0-r0/bin/hi").includes("echo no"))
        fail("a forged archive overwrote the repository's package");

    // Not a zip at all, and a path with nothing behind it.
    put("/home/junk.zip", new TextEncoder().encode("not a zip"));
    prints("pkg install /home/junk.zip", "pkg: /home/junk.zip: archive: invalid", 1);
    prints("pkg install /home/gone.zip", "pkg: /home/gone.zip: read: not found", 1);

    // A URL nothing answers is the network failure it is, told apart from a
    // cross-origin refusal the way every other fetch here is.
    prints("pkg install https://x.example/nothing.zip",
           ["pkg: https://x.example/nothing.zip: read: not found"].join("|"), 1);

    // An operand that is neither is the §6 token it always was.
    prints("pkg install nonesuch", "pkg: nonesuch: not in the index", 1);

    net.routes.delete("https://x.example/other-2.0-r0.zip");
    submit("rm /home/mine-1.0-r0.zip /home/hello.zip /home/bad.zip /home/nometa.zip", at());
    submit("rm /home/odd.zip /home/forged.zip /home/junk.zip", at());
    net.routes.set(RURL + "/index",
                   { status: 200, headers: "content-type: text/plain\n", body: repo("index") });
}

// pkg install: the refusals that write nothing, and the generation, record
// and activation a happy path leaves.
// Part of the smoke suite; test/run.mjs runs the cases in order, the eight
// pkg cases share test/smoke/pkgfix.mjs, and doc/Testing.md has the rules.

import { linkTarget } from "../../web/fs.js";
import { fail, names, net, screen, store, submit, type } from "./harness.mjs";
import {
    IDX, RURL, archive, at, bytes, good, plant, prints, repo, serve, text, update,
} from "./pkgfix.mjs";

export function check() {
    let s = screen();
    // P18. A signed repository of its own — test/unit/repo.data, written
    // by tools/mkrepo.py under keys it destroyed — so index.data and the
    // refusals above keep the bytes they always had. The floor there is
    // 41 and this index is G:1, so /pkg goes first.

    submit("rm -r /pkg", at());
    net.routes.delete(IDX);
    plant("/share/pkg/anchor", repo("anchor"));
    store.dirs.add("/pkg");
    plant("/pkg/repositories", RURL + "\n");
    net.routes.set(RURL + "/index",
                   { status: 200, headers: "content-type: text/plain\n", body: repo("index") });
    serve("libz-1.0-r0", archive("libz-1.0-r0"));
    serve("hello-1.0-r0", archive("hello-1.0-r0"));

    prints("pkg update", `${RURL}|index 1, 2 packages`);

    // A name the index does not offer does not exist (§7 step 7), and
    // nothing is fetched to find that out.
    prints("pkg install nonesuch", "pkg: nonesuch: not in the index", 1);
    if (store.dirs.has("/pkg/gen/1"))
        fail("a refused install built a generation anyway");

    // A package the repository changed after signing, and one longer than
    // the size the index gave. Both refuse by the step they stopped at and
    // leave the store as it was — nothing is unzipped before its hash
    // matches, so there is nothing to undo.
    const tampered = archive("libz-1.0-r0");
    tampered[tampered.length - 20] ^= 1;
    serve("libz-1.0-r0", tampered);
    prints("pkg install libz",
           ["Installing libz (1.0-r0)", "pkg: libz-1.0-r0: digest: permission denied"].join("|"),
           1);
    serve("libz-1.0-r0", Uint8Array.from([...good, 0, 0, 0]));
    prints("pkg install libz",
           ["Installing libz (1.0-r0)", "pkg: libz-1.0-r0: package: invalid"].join("|"), 1);
    if (store.dirs.has("/pkg/store/libz-1.0-r0"))
        fail("a refused package left a store directory behind");
    if (store.files.has("/pkg/db/libz-1.0-r0") || store.files.has("/pkg/active"))
        fail("a refused package was recorded or activated");
    if (store.files.has("/pkg/cache/libz-1.0-r0.zip"))
        fail("a refused package was cached");
    serve("libz-1.0-r0", good);

    // The happy path: hello names libz, both are checked against the index
    // that vouched for them, and one rename commits the pair.
    prints("pkg install hello",
           ["Installing libz (1.0-r0)", "Installing hello (1.0-r0)",
            "generation 1, 2 packages"].join("|"));

    if (text("/pkg/gen/1/packages") !== "hello 1.0-r0\nlibz 1.0-r0\n")
        fail(`the generation holds ${JSON.stringify(text("/pkg/gen/1/packages"))}`);
    if (text("/pkg/world") !== "hello\n")
        fail(`/pkg/world holds ${JSON.stringify(text("/pkg/world"))}`);
    if (linkTarget(bytes("/pkg/active")) !== "/pkg/gen/1")
        fail(`/pkg/active names ${linkTarget(bytes("/pkg/active"))}`);
    if (linkTarget(bytes("/pkg/gen/1/bin/hi")) !== "/pkg/store/hello-1.0-r0/bin/hi")
        fail(`the farm names ${linkTarget(bytes("/pkg/gen/1/bin/hi"))}`);
    if (!text("/pkg/store/hello-1.0-r0/bin/hi").startsWith("#!/bin/sh"))
        fail("the store holds no bin/hi");
    if (store.files.has("/pkg/active.new"))
        fail("the commit left /pkg/active.new behind");

    // §8.1: the stanza as the index gave it, the G that vouched for it,
    // and a digest per file.
    const record = text("/pkg/db/hello-1.0-r0");
    for (const want of ["P:hello\n", "V:1.0-r0\n", "G:1\n", "F:bin\n", "R:hi\n", "Z:Q2"])
        if (!record.includes(want))
            fail(`/pkg/db/hello-1.0-r0 carries no ${JSON.stringify(want)}`);

    // The whole of activation: /pkg/bin is the second component of the
    // default search list, and nothing was told a generation appeared.
    prints("hi", "hi from hello");

    // The cache is the archive that passed, re-hashed rather than
    // believed: with nothing answering, a second install still resolves.
    if (!store.files.has("/pkg/cache/hello-1.0-r0.zip"))
        fail("the install cached no archive");
    net.routes.delete(`${RURL}/hello-1.0-r0.zip`);
    net.routes.delete(`${RURL}/libz-1.0-r0.zip`);
    prints("pkg install hello", "generation 1, unchanged");
    if (store.dirs.has("/pkg/gen/2"))
        fail("an install with nothing to do built a generation");

    // Rolled back the way §8.3 says to roll one back — the link swung off
    // the generation — and then the repository republishes libz-1.0-r0
    // under another digest. The store directory is still there and §8
    // makes it immutable, so the transaction stops rather than write over
    // what the generation it may be swung back to is running out of.
    submit("rm /pkg/active", at());
    plant("/pkg/db/libz-1.0-r0", text("/pkg/db/libz-1.0-r0").replace(/^C:Q2./m, "C:Q2A"));
    // World still names hello, so with no generation both come back — and
    // the plan is printed whole before any of it is performed.
    prints("pkg install libz",
           ["Installing libz (1.0-r0)", "Installing hello (1.0-r0)",
            "pkg: libz-1.0-r0: installed at a different digest"].join("|"), 1);
    if (store.files.has("/pkg/active") || store.dirs.has("/pkg/gen/2"))
        fail("a refused replacement activated a generation");
}

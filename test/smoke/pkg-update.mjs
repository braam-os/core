// pkg update: Package_Management.md §7's pipeline behind one word, and the
// index it leaves — read back by search, info and list.
// Part of the smoke suite; test/run.mjs runs the cases in order, the eight
// pkg cases share test/smoke/pkgfix.mjs, and doc/Testing.md has the rules.

import { E } from "../../web/abi.js";
import {
    fail, net, output, prompt, regrid, resize, row, rows, run, screen, store, submit,
} from "./harness.mjs";
import {
    IDX, REPO, anchors, at, fixture, good, keep, plant, prints, refuses, route, update,
} from "./pkgfix.mjs";

export function check() {
    let s = screen();
    // Keeps the release's anchor before a fixture goes over it; pkg-crash
    // puts it back.
    keep();

    // Nothing to update from: /pkg does not exist, and the release's own
    // /etc/repositories is emptied first, since a file that is not there and
    // one with nothing in it read alike (Package_Format.md §8.2).
    plant("/etc/repositories", "");
    s = update();
    if (!rows(s).some((line) => line.startsWith("pkg: no repositories")))
        fail(`pkg update with no repositories printed ${JSON.stringify(output(s))}`);
    if (!rows(s).includes(prompt(1)))
        fail(`pkg update with no repositories left ${row(s, s.cursor_y)}`);

    plant("/etc/anchor", fixture("anchor"));
    store.dirs.add("/pkg");
    plant("/etc/repositories", REPO + "\n");

    // P16's three read what an update left, and there has been none.
    submit("clear", at());
    s = submit("pkg search awk", at());
    if (!rows(s).includes("pkg: no index; run pkg update"))
        fail(`pkg search with no index printed ${JSON.stringify(output(s))}`);
    if (!rows(s).includes(prompt(1)))
        fail(`pkg search with no index left ${row(s, s.cursor_y)}`);

    // Each of §7's steps, named by the one that refused. Nothing is stored
    // by any of them.
    net.routes.delete(IDX);
    refuses("a repository nothing answers for", "pkg: fetch:");
    route(fixture("tampered"));
    refuses("an index changed after signing", "pkg: signature:");
    route(fixture("grammar"));
    refuses("an index in a grammar this is not", "pkg: header:");
    route(fixture("foreign"));
    refuses("an index for another repository", "pkg: header:");
    route(fixture("expired"));
    refuses("an expired index", "pkg: expiry:");
    // §2: a Y: naming a key the anchor does not carry counts for nothing.
    route(fixture("stranger"));
    refuses("an index signed by a key the anchor does not name", "pkg: signature:");
    // §7 step 7's other half: the file is signed and current, and one
    // record in it is not a record.
    route(fixture("malformed"));
    refuses("an index carrying a line that is not a field", "pkg: read:");

    // P27. Step 4's counting rule, one step earlier: an anchor meets its
    // own H:root or it is not an anchor (§4). anchor.data is a key set of
    // its own, so these refuse before anything is fetched.
    route(fixture("good"));
    plant("/etc/anchor", anchors("repeat"));
    refuses("an anchor meeting its threshold with one key twice", "pkg: anchor:");
    plant("/etc/anchor", anchors("short"));
    refuses("an anchor one signature short", "pkg: anchor:");
    // The control the pair needs: three signatures, one of them a
    // stranger's and two of them counting. The anchor is taken, and the
    // refusal moves on to the index it does not vouch for.
    plant("/etc/anchor", anchors("extra"));
    refuses("an index under an anchor that holds no key of its", "pkg: signature:");
    plant("/etc/anchor", fixture("anchor"));

    if (store.files.has("/pkg/index"))
        fail("a refused update recorded an index anyway");

    // The record, which is the whole of what P15 adds to §7.
    route(fixture("good"));
    s = update();
    if (output(s).join("|") !== `${REPO}|index 41, 2 packages`)
        fail(`pkg update printed ${JSON.stringify(output(s))}`);
    if (!rows(s).includes(prompt()))
        fail(`pkg update left ${row(s, s.cursor_y)}, expected ${prompt()}`);
    const stored = new TextDecoder().decode(store.files.get("/pkg/index") || new Uint8Array(0));
    if (stored !== fixture("good"))
        fail("/pkg/index is not the file that was checked, signature block and all");

    // The same version again is nothing to do, and not an error.
    s = update();
    if (output(s).join("|") !== `${REPO}|index 41, unchanged`)
        fail(`a second update printed ${JSON.stringify(output(s))}`);
    if (!rows(s).includes(prompt()))
        fail(`a second update left ${row(s, s.cursor_y)}, expected ${prompt()}`);

    // -v traces the fetch: `>` what was asked for, `<` what came back. Read
    // through a pipe, so a line the grid would wrap cannot make this brittle,
    // and the flag is taken wherever it was typed.
    prints("pkg update -v 2>&1 | grep '> GET'", `> GET ${IDX}`);
    prints("pkg -v update 2>&1 | grep '> GET'", `> GET ${IDX}`);
    prints("pkg update -v 2>&1 | grep '< 200'", "< 200");
    prints("pkg update -v 2>&1 | grep content-type", "< content-type: text/plain");
    // The body's size is counted as it is read, so the fixture's own length
    // is the number.
    prints("pkg update -v 2>&1 | grep bytes", `< ${fixture("good").length} bytes`);

    // P16: the index read back, and the generation beside it. A listing is
    // wider than this grid, so it goes wide the way help and ps do.
    regrid(100, 24, "the resize before pkg search failed");

    // The columns are measured over the matches, and a package with no
    // description leaves the version column unpadded.
    prints("pkg search '*'",
           "awk   1.2-r0  pattern-directed scanning and processing language|less  1.6-r1");
    // A pattern with no metacharacter is a substring, over both fields and
    // ignoring case; one with a metacharacter globs the whole field.
    prints("pkg search awk", "awk  1.2-r0  pattern-directed scanning and processing language");
    prints("pkg search LESS", "less  1.6-r1");
    prints("pkg search '*scan*'",
           "awk  1.2-r0  pattern-directed scanning and processing language");
    prints("pkg search 'aw?'", "awk  1.2-r0  pattern-directed scanning and processing language");
    // Nothing matched is nothing printed, and still a success.
    prints("pkg search nonesuch", "");

    // §3.3's order with C last, and no row for a field the stanza has not
    // got: the fixture's `less` carries no description.
    prints("pkg info less",
           ["name          less", "version       1.6-r1", "size          9000",
            "digest        Q2IgfM18bBUW8blv5C1wE491Z5bfWNc+VRhcgcX1hLHUI="].join("|"));
    // §7 step 7: a name the index does not list does not exist.
    prints("pkg info nonesuch", "pkg: nonesuch: not in the index", 1);

    // No generation is nothing installed, and not an error.
    prints("pkg list", "");

    // One planted by hand, since installing is P18. `pkg list` reads the
    // link and the text behind it, and `info` gains a row.
    submit("mkdir -p /pkg/gen/1", at());
    submit("echo awk 1.2-r0 > /pkg/gen/1/packages", at());
    submit("echo less 1.6-r1 >> /pkg/gen/1/packages", at());
    submit("ln -s /pkg/gen/1 /pkg/active", at());
    prints("pkg list", "awk   1.2-r0|less  1.6-r1");
    prints("pkg info awk",
           ["name          awk", "version       1.2-r0", "installed     1.2-r0",
            "size          18244", "unpacked      41984",
            "description   pattern-directed scanning and processing language",
            "depends       cmd:sh", "provides      cmd:awk",
            "digest        Q2IgfM18bBUW8blv5C1wE491Z5bfWNc+VRhcgcX1hLHUI="].join("|"));

    // A usage error is 2, the same as any other program's.
    prints("pkg list please", "Usage: pkg list", 2);
    prints("pkg info", "Usage: pkg info <package>", 2);

    regrid(60, 16, "the resize after pkg info failed");

    // And now that 41 is the floor, an older index is a rollback.
    route(fixture("older"));
    refuses("an index whose version went backwards", "pkg: version:");
    if (new TextDecoder().decode(store.files.get("/pkg/index")) !== fixture("good"))
        fail("a rollback overwrote the stored index");

    // The two ways a browser refuses a fetch, which the Error byte cannot
    // tell apart: the hint does, in curl's words (net.mjs asserts the pair
    // for curl). -v says the same thing on the `<` side.
    net.routes.set(IDX, { fail: E.PERM });
    s = update();
    if (!rows(s).some((line) => line.startsWith("pkg: fetch: permission denied")))
        fail(`a refused origin printed ${JSON.stringify(output(s))}`);
    if (!rows(s).some((line) => line.includes("cross-origin")))
        fail(`a refused origin said nothing about CORS: ${JSON.stringify(output(s))}`);
    // The status here is grep's, the refusal having gone down the pipe.
    prints("pkg update -v 2>&1 | grep '< no response'", "< no response: permission denied");

    net.routes.set(IDX, { fail: E.IO });
    s = update();
    if (!rows(s).some((line) => line.startsWith("pkg: no answer")))
        fail(`a dead network printed ${JSON.stringify(output(s))}`);
    route(fixture("good"));

    // One repository, for now: /pkg/index is one file and the floor is one
    // number, so a second line is refused rather than ignored.
    plant("/etc/repositories", REPO + "\n" + REPO + "\n");
    s = update();
    if (!rows(s).some((line) => line.startsWith("pkg: /etc/repositories:")))
        fail(`a second repository printed ${JSON.stringify(output(s))}`);
    if (!rows(s).includes(prompt(1)))
        fail(`a second repository left ${row(s, s.cursor_y)}`);
}

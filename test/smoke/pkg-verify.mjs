// pkg verify: §8.1's record read back against the bytes the store holds.
// Part of the smoke suite; test/run.mjs runs the cases in order, the eight
// pkg cases share test/smoke/pkgfix.mjs, and doc/Testing.md has the rules.

import { fail, net, screen, store, submit, type } from "./harness.mjs";
import { RURL, at, plant, prints, repo, text, update } from "./pkgfix.mjs";

export function check() {
    let s = screen();
    // P21. §8.1's record read back: the paths it names, and the bytes
    // behind them hashed again. A tree of its own, since what is checked
    // is what an install has just written.
    submit("rm -r /pkg", at());
    store.dirs.add("/pkg");
    plant("/pkg/repositories", RURL + "\n");
    net.routes.set(RURL + "/index",
                   { status: 200, headers: "content-type: text/plain\n", body: repo("index") });
    prints("pkg update", `${RURL}|index 1, 2 packages`);
    prints("pkg install hello",
           ["Installing libz (1.0-r0)", "Installing hello (1.0-r0)",
            "generation 1, 2 packages"].join("|"));

    // The F groups in the order the unpack wrote them, as absolute store
    // paths: what `verify` names, and what `cat` takes.
    prints("pkg files hello",
           ["/pkg/store/hello-1.0-r0/bin/hi",
            "/pkg/store/hello-1.0-r0/share/hello/greeting"].join("|"));
    prints("pkg files libz", "/pkg/store/libz-1.0-r0/share/libz/README");
    prints("pkg files nonesuch", "pkg: nonesuch: not installed", 1);
    prints("pkg files", "usage: pkg files <package>", 2);
    prints("pkg files hello libz", "usage: pkg files <package>", 2);

    // Nothing wrong is nothing printed. This is the case that says the
    // digests being compared are the ones the install recorded.
    prints("pkg verify", "");

    // A file the store still has, at other bytes. The walk is recursive,
    // so a nested F is reached too.
    submit("echo tampered >> /pkg/store/hello-1.0-r0/bin/hi", at());
    submit("echo tampered > /pkg/store/hello-1.0-r0/share/hello/greeting", at());
    prints("pkg verify",
           ["modified  /pkg/store/hello-1.0-r0/bin/hi",
            "modified  /pkg/store/hello-1.0-r0/share/hello/greeting"].join("|"), 1);

    // An operand scopes it, and libz was never touched.
    prints("pkg verify libz", "");
    prints("pkg verify hello",
           ["modified  /pkg/store/hello-1.0-r0/bin/hi",
            "modified  /pkg/store/hello-1.0-r0/share/hello/greeting"].join("|"), 1);

    // Gone, and one the record does not name.
    submit("rm /pkg/store/hello-1.0-r0/bin/hi", at());
    submit("echo x > /pkg/store/hello-1.0-r0/bin/spare", at());
    prints("pkg verify hello",
           ["missing   /pkg/store/hello-1.0-r0/bin/hi",
            "modified  /pkg/store/hello-1.0-r0/share/hello/greeting",
            "extra     /pkg/store/hello-1.0-r0/bin/spare"].join("|"), 1);

    // §11: it says a file changed, and it changes nothing back.
    if (!store.files.has("/pkg/store/hello-1.0-r0/bin/spare"))
        fail("pkg verify removed a file it only reported");
    prints("pkg list", "hello  1.0-r0|libz   1.0-r0");

    prints("pkg verify nonesuch", "pkg: nonesuch: not installed", 1);
    prints("pkg verify hello libz", "usage: pkg verify [<package>]", 2);

    // Nothing installed is nothing to verify, and not an error.
    submit("rm -r /pkg", at());
    prints("pkg verify", "");
    prints("pkg files hello", "pkg: hello: not installed", 1);
}

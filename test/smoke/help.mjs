// /etc/help against the builtin table and the archive, and the anchor's expiry.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import { readFileSync } from "node:fs";

import { fail, hasRootfs, names, store } from "./harness.mjs";

export function check() {
    // /etc/help is the whole of `help` now — /bin/help pages it — so nothing
    // in the system notices when it goes stale. This does: every builtin the
    // table carries and every binary the archive ships is named once, and the
    // document names nothing else. The archive rather than the source tree,
    // since what ships is what a reader gets.
    if (hasRootfs) {
        const doc = store.entries.find((e) => e.name === "etc/help");
        if (!doc)
            fail("the archive carries no etc/help");
        const text = new TextDecoder().decode(doc.bytes);

        // A section is a heading at the left margin; an entry inside one is two
        // spaces and a name. A continuation line is indented past that.
        const section = (heading) => {
            const at = text.indexOf(`\n${heading}\n`);
            if (at < 0)
                fail(`/etc/help has no ${JSON.stringify(heading)} section`);
            const rest = text.slice(at + heading.length + 2);
            const end = rest.search(/\n[^\s-]/);
            return (end < 0 ? rest : rest.slice(0, end))
                .split("\n")
                .map((line) => /^ {2}(\S+)\s/.exec(line))
                .filter(Boolean)
                .map((m) => m[1]);
        };

        const table = readFileSync(new URL("../../src/cmd/sh/builtin/table.cpp", import.meta.url),
                                   "utf8");
        const listed = {
            "Shell builtins": [...table.matchAll(/^ {4}\{ "(.+?)", builtin_/gm)].map((m) => m[1]),
            "Programs in /bin": store.entries.filter((e) => /^bin\/[^/]+$/.test(e.name))
                                             .map((e) => e.name.slice(4)),
        };
        for (const [heading, want] of Object.entries(listed)) {
            const got = section(heading);
            if (want.length === 0)
                fail(`nothing to check ${heading} against`);
            for (const name of want)
                if (got.filter((n) => n === name).length !== 1)
                    fail(`/etc/help names ${name} ${got.filter((n) => n === name).length} ` +
                         `times under ${heading}, expected once`);
            for (const name of got)
                if (!want.includes(name))
                    fail(`/etc/help lists ${name} under ${heading}, which is not there`);
        }
    }

    // The trust anchor ships in the archive and stops working when it expires
    // (Package_Format.md §4). This is the only place a real clock sees it, so
    // it is what says so before a release goes out with a stale one.
    if (hasRootfs) {
        const a = store.entries.find((e) => e.name === "etc/anchor");
        if (!a)
            fail("the archive carries no etc/anchor");
        const m = new TextDecoder().decode(a.bytes).match(/\nE:(\d+)\n/);
        if (!m)
            fail("/etc/anchor carries no expiry");
        else if (Number(m[1]) <= Date.now())
            fail(`/etc/anchor expired at ${new Date(Number(m[1])).toISOString()}`);
    }
}

// What the nine pkg cases share: the fixtures, the two repositories they are
// served from, the clock they run on, and the four shapes they assert in.
//
// The fixtures are the unit suite's — an anchor over throwaway keys and the
// indexes signed under it — so both suites check one set of bytes. The shipped
// anchor's keys were destroyed when it was signed, so a happy path means
// planting one of these over it; what that proves is the machinery, not the
// release's keys. pkg-crash puts the shipped one back.

import { readFileSync } from "node:fs";

import { fail, net, output, prompt, row, rows, shows, store, submit } from "./harness.mjs";

// The C suite's fixtures, read the way it reads them: one @name block to the
// next. Three files here, so one reader over all three.
const reader = (file) => {
    const data = readFileSync(new URL(`../unit/${file}.data`, import.meta.url), "utf8")
        .replace(/^R"DATA\(/, "").replace(/\)DATA"\n$/, "");
    return (name) => {
        const key = `\n@${name}\n`;
        const at = data.indexOf(key);
        if (at < 0)
            fail(`test/unit/${file}.data carries no @${name}`);
        const rest = data.slice(at + key.length);
        const end = rest.indexOf("\n@");
        return end < 0 ? rest : rest.slice(0, end + 1);
    };
};

export const fixture = reader("index");
export const anchors = reader("anchor");
export const repo = reader("repo");

// The first repository: index.data, whose floor is 41, served as text.
export const REPO = "https://packages.example/braam";
export const IDX = REPO + "/index";

// The second: repo.data, written by tools/mkrepo.py under keys it destroyed,
// so index.data and the refusals over it keep the bytes they always had.
export const RURL = "https://packages.test/braam";

export const plant = (path, text) => store.files.set(path, new TextEncoder().encode(text));

export const route = (body) =>
    net.routes.set(IDX, { status: 200, headers: "content-type: text/plain\n", body });

export const archive = (stem) =>
    Uint8Array.from(Buffer.from(repo(`pkg-${stem}`).replace(/\n/g, ""), "base64"));

export const serve = (stem, body) =>
    net.routes.set(`${RURL}/${stem}.zip`,
                   { status: 200, headers: "content-type: application/zip\n", body });

// The untampered libz archive, re-served by six of the eight cases.
export const good = archive("libz-1.0-r0");

export const bytes = (path) => store.files.get(path);

export const text = (path) => {
    const b = bytes(path);
    if (!b)
        fail(`${path} is not there`);
    return new TextDecoder().decode(b);
};

// The release's own /etc files, kept before a fixture is planted over them.
// Held here rather than in a case because the case that puts them back is not
// the one that took them.
const SHIPPED = ["/etc/anchor", "/etc/repositories"];
let shipped = null;
export const keep = () => { shipped = SHIPPED.map((p) => store.files.get(p)); };
export const restore = () => SHIPPED.forEach((p, i) => store.files.set(p, shipped[i]));

// One clock for the nine, since they are one session: a case picks up where
// the one before it left off.
export const { at } = shows(1184.8);

// The two lines an update prints, or the rows it left behind.
export const update = () => {
    submit("clear", at());
    return submit("pkg update", at());
};

export const refuses = (what, want) => {
    const got = update();
    if (!rows(got).includes(REPO))
        fail(`${what} did not name the repository: ${JSON.stringify(output(got))}`);
    if (!rows(got).some((line) => line.startsWith(want)))
        fail(`${what} printed ${JSON.stringify(output(got))}, expected ${want}`);
    if (!rows(got).includes(prompt(1)))
        fail(`${what} left ${row(got, got.cursor_y)}, expected ${prompt(1)}`);
};

export const query = (line) => {
    submit("clear", at());
    return submit(line, at());
};

export const prints = (line, want, status = 0) => {
    const got = query(line);
    if (output(got).join("|") !== want)
        fail(`${line} printed ${JSON.stringify(output(got))}, expected ${want}`);
    if (!rows(got).includes(prompt(status)))
        fail(`${line} left ${row(got, got.cursor_y)}, expected ${prompt(status)}`);
};

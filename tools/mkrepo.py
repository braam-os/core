#!/usr/bin/env python3
"""The signed repository test/run.mjs installs from.

    mkrepo.py [<out>]

Generates throwaway keys in a temporary directory, builds an anchor, two
packages and an index over them, writes test/unit/repo.data, and destroys the
keys. Package_Management.md §9 stays true: the tree holds no private key, and
what is checked in names keys nobody has.

Re-running it re-signs everything under new keys, so the whole file changes;
that is the same bargain test/unit/index.data made.
"""

import base64
import sys
import tempfile
from pathlib import Path

import mkanchor
import mkindex
import mkpkg

URL = "https://packages.test/braam"

# The fixtures carry their own clock, so they do not rot. fakesvc's now is
# 1782000000000.
ANCHOR_EXPIRY = 3000000000000
INDEX_EXPIRY = 2000000000000

# A `#!` file is a program here, so a fixture needs no binary.
LIBZ = {
    "name": "libz",
    "version": "1.0-r0",
    "fields": {"T": "a compression library, as far as anyone knows"},
    "files": {"share/libz/README": "nothing is compressed here\n"},
}


def hello(version, greeting):
    return {
        "name": "hello",
        "version": version,
        "fields": {"T": "a greeting", "D": "libz", "p": "cmd:hi"},
        "files": {
            "bin/hi": f"#!/bin/sh\necho {greeting}\n",
            "share/hello/greeting": greeting + "\n",
        },
    }


# §5.1's six scripts, each saying which it is and what it was handed. 1.1's
# fail: post-upgrade for a mark written after the commit, pre-install for one
# written before it, which the commit must not overwrite.
def noisy(version, failing):
    files = {"bin/noisy": f"#!/bin/sh\necho noisy {version}\n"}
    for name in ("pre-install", "post-install", "pre-deinstall", "post-deinstall",
                 "pre-upgrade", "post-upgrade"):
        tail = "exit 1\n" if name in failing else ""
        files[f".{name}"] = f"echo {name} $*\n" + tail
    return {"name": "noisy", "version": version, "fields": {"T": "a package that talks"},
            "files": files}


PACKAGES = [LIBZ, hello("1.0-r0", "hi from hello"), hello("1.1-r0", "hi from hello 1.1"),
            noisy("1.0-r0", ()), noisy("1.1-r0", ("post-upgrade", "pre-install"))]

# One index per generation of the repository: the second moves hello and
# leaves libz where it is, so an upgrade can be seen touching only what moved.
# The last two are noisy's alone, so the assertions over the first two are not
# disturbed by a package that exists to make noise.
INDEXES = [
    (1, ["libz-1.0-r0", "hello-1.0-r0"]),
    (2, ["libz-1.0-r0", "hello-1.1-r0"]),
    (3, ["noisy-1.0-r0"]),
    (4, ["noisy-1.1-r0"]),
]


def build(tmp: Path):
    keys = {}
    for name in ("root1", "root2", "root3", "index"):
        keys[name] = tmp / f"{name}.key"
        mkanchor.ed25519.generate(keys[name])

    anchor = tmp / "anchor"
    mkanchor.main([
        "mkanchor.py", "--out", str(anchor), "--version", "1",
        "--expiry", str(ANCHOR_EXPIRY),
        "--threshold", "root=2", "--threshold", "index=1",
        "--key", f"root={keys['root1']}", "--key", f"root={keys['root2']}",
        "--key", f"root={keys['root3']}", "--key", f"index={keys['index']}",
        "--sign", str(keys["root1"]), "--sign", str(keys["root2"]),
    ])

    zips = []
    for p in PACKAGES:
        stem = f"{p['name']}-{p['version']}"
        argv = ["mkpkg.py", "--out", str(tmp / f"{stem}.zip"),
                "--name", p["name"], "--version", p["version"]]
        for letter, value in p["fields"].items():
            argv += ["--field", f"{letter}={value}"]
        for entry, text in p["files"].items():
            src = tmp / f"{stem}_{entry.replace('/', '_')}"
            src.write_text(text)
            argv.append(f"{src}={entry}")
        mkpkg.main(argv)
        zips.append(tmp / f"{stem}.zip")

    blocks = [("anchor", anchor.read_text())]
    for version, stems in INDEXES:
        index = tmp / f"index{version}"
        mkindex.main([
            "mkindex.py", "--out", str(index), "--url", URL,
            "--version", str(version), "--expiry", str(INDEX_EXPIRY),
            "--description", "Braam test packages", "--sign", str(keys["index"]),
        ] + [str(tmp / f"{stem}.zip") for stem in stems])
        blocks.append(("index" if version == 1 else f"index{version}", index.read_text()))

    for z in zips:
        wrapped = base64.b64encode(z.read_bytes()).decode()
        rows = "\n".join(wrapped[i:i + 76] for i in range(0, len(wrapped), 76))
        blocks.append((f"pkg-{z.stem}", rows + "\n"))
    return blocks


def main(argv):
    if len(argv) > 2:
        sys.exit("usage: mkrepo.py [<out>]")
    out = Path(argv[1]) if len(argv) == 2 else \
        Path(__file__).resolve().parent.parent / "test" / "unit" / "repo.data"

    with tempfile.TemporaryDirectory() as dir:
        blocks = build(Path(dir))

    text = 'R"DATA(\n' + "".join(f"@{name}\n{body}" for name, body in blocks) + ')DATA"\n'
    out.write_text(text)
    print(f"{out}: {len(blocks)} blocks, {len(text)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

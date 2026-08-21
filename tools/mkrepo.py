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
PACKAGES = [
    {
        "name": "libz",
        "version": "1.0-r0",
        "fields": {"T": "a compression library, as far as anyone knows"},
        "files": {"share/libz/README": "nothing is compressed here\n"},
    },
    {
        "name": "hello",
        "version": "1.0-r0",
        "fields": {"T": "a greeting", "D": "libz", "p": "cmd:hi"},
        "files": {
            "bin/hi": "#!/bin/sh\necho hi from hello\n",
            "share/hello/greeting": "hello\n",
        },
    },
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
        argv = ["mkpkg.py", "--out", str(tmp / f"{p['name']}-{p['version']}.zip"),
                "--name", p["name"], "--version", p["version"]]
        for letter, value in p["fields"].items():
            argv += ["--field", f"{letter}={value}"]
        for entry, text in p["files"].items():
            src = tmp / entry.replace("/", "_")
            src.write_text(text)
            argv.append(f"{src}={entry}")
        mkpkg.main(argv)
        zips.append(tmp / f"{p['name']}-{p['version']}.zip")

    index = tmp / "index"
    mkindex.main([
        "mkindex.py", "--out", str(index), "--url", URL, "--version", "1",
        "--expiry", str(INDEX_EXPIRY), "--description", "Braam test packages",
        "--sign", str(keys["index"]),
    ] + [str(z) for z in zips])

    blocks = [("anchor", anchor.read_text()), ("index", index.read_text())]
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

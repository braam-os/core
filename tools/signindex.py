#!/usr/bin/env python3
"""Sign a signed document: an index (§3), an anchor (§4), anything with §2's
shape.

    signindex.py <body> <key>...

`body` is the file *without* its signature block, and is signed whole. One
`Y:` line per key goes to stdout and nothing else; the caller puts them in
front of the body with an empty line between, which is the split
`signed_split` in src/cmd/pkg/stanza.cpp makes — the one place a publisher and
a client can disagree in silence. A body holds empty lines of its own, so the
file handed in here is never the assembled document.
"""

import sys

import ed25519


def y_line(private, message):
    public = ed25519.public_of(private)
    return (f"Y:ed25519 {ed25519.key_name(public)} "
            f"{ed25519.b64(ed25519.sign(private, message))}")


def main(argv):
    if len(argv) < 3:
        sys.exit("usage: signindex.py <body> <key>...")

    with open(argv[1], "rb") as f:
        message = f.read()

    for path in argv[2:]:
        print(y_line(ed25519.load(path), message))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

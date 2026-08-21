#!/usr/bin/env python3
"""The anchor (Package_Format.md §4), signed by a threshold of its own roots.

    mkanchor.py --out <file> --version <G> --expiry <ms>
                --threshold <use>=<n>... --key <use>=<path>...
                --sign <path>...

`--key` names the public halves to write down, `--sign` the private halves to
sign with. §9: nothing here keeps a key, and the private halves it is pointed
at do not have to be the ones it writes down.
"""

import sys

import ed25519
import signindex


def parse(argv):
    out, version, expiry = None, None, None
    thresholds, keys, signers = [], [], []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a in ("--out", "--version", "--expiry", "--threshold", "--key", "--sign"):
            if i + 1 >= len(argv):
                sys.exit(f"mkanchor.py: {a} wants a value")
            v = argv[i + 1]
            i += 2
            if a == "--out":
                out = v
            elif a == "--version":
                version = int(v)
            elif a == "--expiry":
                expiry = int(v)
            elif a == "--threshold":
                use, _, n = v.partition("=")
                thresholds.append((use, int(n)))
            elif a == "--key":
                use, _, path = v.partition("=")
                keys.append((use, path))
            else:
                signers.append(v)
        else:
            sys.exit(f"mkanchor.py: unknown argument {a}")
    if out is None or version is None or expiry is None:
        sys.exit("mkanchor.py: --out, --version and --expiry are required")
    return out, version, expiry, thresholds, keys, signers


def body(version, expiry, thresholds, keys):
    lines = [f"X:1", f"G:{version}", f"E:{expiry}"]
    lines += [f"H:{use} {n}" for use, n in thresholds]
    for use, path in keys:
        public = ed25519.public_of(ed25519.load(path))
        lines.append(f"K:{use} ed25519 {ed25519.b64(public)}")
    return "\n".join(lines) + "\n"


def main(argv):
    out, version, expiry, thresholds, keys, signers = parse(argv)

    text = body(version, expiry, thresholds, keys)
    block = [signindex.y_line(ed25519.load(p), text.encode()) for p in signers]
    with open(out, "w") as f:
        f.write("\n".join(block) + "\n\n" + text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

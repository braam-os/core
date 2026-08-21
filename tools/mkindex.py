#!/usr/bin/env python3
"""The repository index (Package_Formats.md §3), signed.

    mkindex.py --out <file> --url <N> --version <G> --expiry <ms>
               [--description <T>] [--sign <key>]... <package.zip>...

`C` and `S` are taken from the bytes, §6.1's `cmd:` names from the zip's `bin/`,
and the rest of a stanza is read out of the package's own `.PKGINFO`.
Package_Management.md §7: a URL proves nothing, so no record names one — a
package is at `<N>/<name>-<version>.zip`.
"""

import sys
import zipfile
from pathlib import Path

import ed25519
import signindex

# §3.3's canonical order.
ORDER = "CPVSITotkgDpi"

# §6's operator characters, and the separators §6's lists are split on. A `bin/`
# entry carrying one of these cannot be written as a dependency token.
UNTOKENISABLE = set("<>=~ \t\n")


def commands(z, path):
    """§6.1's names: every flat entry of `bin/`, directories skipped."""
    names = []
    for entry in z.namelist():
        if not entry.startswith("bin/"):
            continue
        cmd = entry[4:]
        if not cmd or "/" in cmd:
            continue
        if UNTOKENISABLE & set(cmd):
            sys.exit(f"mkindex.py: {path}: bin/{cmd} is not a dependency name")
        names.append(cmd)
    return sorted(names)


def parse(argv):
    out, url, version, expiry, description = None, None, None, None, None
    signers, zips = [], []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a in ("--out", "--url", "--version", "--expiry", "--description", "--sign"):
            if i + 1 >= len(argv):
                sys.exit(f"mkindex.py: {a} wants a value")
            v = argv[i + 1]
            i += 2
            if a == "--out":
                out = v
            elif a == "--url":
                url = v.rstrip("/")
            elif a == "--version":
                version = int(v)
            elif a == "--expiry":
                expiry = int(v)
            elif a == "--description":
                description = v
            else:
                signers.append(v)
        else:
            zips.append(a)
            i += 1
    if out is None or url is None or version is None or expiry is None:
        sys.exit("mkindex.py: --out, --url, --version and --expiry are required")
    return out, url, version, expiry, description, signers, zips


def stanza(path):
    raw = Path(path).read_bytes()
    with zipfile.ZipFile(path) as z:
        info = z.read(".PKGINFO").decode()
        cmds = commands(z, path)

    fields = {"C": ed25519.digest(raw), "S": str(len(raw))}
    for line in info.splitlines():
        if len(line) > 1 and line[1] == ":":
            fields[line[0]] = line[2:]
    for want in ("P", "V"):
        if want not in fields:
            sys.exit(f"mkindex.py: {path}: .PKGINFO carries no {want}")

    # §6.1: a hand-written `p:` is an ordinary provide and merges with them.
    provides = [fields["p"]] if fields.get("p") else []
    provides += [f"cmd:{c}={fields['V']}" for c in cmds]
    if provides:
        fields["p"] = " ".join(provides)
    return "".join(f"{k}:{fields[k]}\n" for k in ORDER if k in fields)


def main(argv):
    out, url, version, expiry, description, signers, zips = parse(argv)

    head = [f"X:1", f"N:{url}", f"G:{version}", f"E:{expiry}"]
    if description is not None:
        head.append(f"T:{description}")
    body = "\n".join(head) + "\n\n" + "\n".join(stanza(z) for z in zips)

    block = [signindex.y_line(ed25519.load(p), body.encode()) for p in signers]
    with open(out, "w") as f:
        f.write("\n".join(block) + "\n\n" + body if block else body)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

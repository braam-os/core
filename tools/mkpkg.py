#!/usr/bin/env python3
"""A package zip (Package_Format.md §5).

    mkpkg.py --out <file> --name <n> --version <v> [--field <L>=<value>]...
             <src>=<entry>...

`.PKGINFO` is written from the fields; every `<src>=<entry>` pair is payload.
Reproducible: `stamp` and `MODE` are pack.py's rather than restated here.
"""

import sys
import zipfile
from pathlib import Path

from pack import MODE, stamp

# §3.3's order, which .PKGINFO shares.
ORDER = "PVITotkgDpi"


def parse(argv):
    out, fields, payload = None, {}, []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a in ("--out", "--name", "--version", "--field"):
            if i + 1 >= len(argv):
                sys.exit(f"mkpkg.py: {a} wants a value")
            v = argv[i + 1]
            i += 2
            if a == "--out":
                out = v
            elif a == "--name":
                fields["P"] = v
            elif a == "--version":
                fields["V"] = v
            else:
                letter, _, value = v.partition("=")
                fields[letter] = value
        elif "=" in a:
            src, _, entry = a.partition("=")
            payload.append((entry, src))
            i += 1
        else:
            sys.exit(f"mkpkg.py: unknown argument {a}")
    if out is None or "P" not in fields or "V" not in fields:
        sys.exit("mkpkg.py: --out, --name and --version are required")
    return out, fields, sorted(payload)


def pkginfo(fields, unpacked):
    fields = dict(fields)
    fields.setdefault("I", str(unpacked))
    return "".join(f"{k}:{fields[k]}\n" for k in ORDER if k in fields)


def main(argv):
    out, fields, payload = parse(argv)

    bodies = [(entry, Path(src).read_bytes()) for entry, src in payload]
    when = stamp()

    with zipfile.ZipFile(out, "w") as z:
        for name, data in [(".PKGINFO",
                            pkginfo(fields, sum(len(b) for _, b in bodies)).encode())] + bodies:
            info = zipfile.ZipInfo(name, when)
            info.external_attr = MODE
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

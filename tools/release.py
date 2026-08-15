#!/usr/bin/env python3
"""Pack a built tree into a versioned archive: the site, or the SDK.

    release.py <dir> <outdir> --version-file <version.h>
               [--name <stem>] [--require <path>...] [--extra <file>...]

The archive is <stem>-<version>.zip, holding one directory of that name, so it
unpacks beside whatever else a web root already has. Entries are sorted and
timestamped alike, so one tree gives one archive, byte for byte.
"""

import argparse
import sys
import zipfile
from pathlib import Path

# version.py sits beside this script, which is sys.path[0] as it is run.
from version import version_of

# What a built site cannot be missing.
REQUIRED = ("index.html", "kernel.wasm", "bundle.bin")

# The tier measurement's own files, which land in build/web/ so they can be
# served and are no part of the site. Names, not a pattern: an exclusion that
# guesses is worse than one that is spelled.
SKIP = {"bench.html", "bench.js", "bundle3.bin", "bundle3nosh.bin"}

# Fixed, because a zip timestamp is the difference between two identical
# builds. 1980-01-01 is the earliest a zip can express.
STAMP = (1980, 1, 1, 0, 0, 0)
MODE = 0o100644 << 16


def collect(root: Path, extras):
    files = [(p.relative_to(root).as_posix(), p) for p in sorted(root.rglob("*"))
             if p.is_file() and p.name not in SKIP]
    files += [(p.name, p) for p in extras]
    return sorted(files)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path)
    ap.add_argument("outdir", type=Path)
    ap.add_argument("--version-file", type=Path, required=True)
    ap.add_argument("--name", default="braam")
    ap.add_argument("--require", action="append", default=None)
    ap.add_argument("--extra", type=Path, action="append", default=[])
    args = ap.parse_args(argv[1:])

    required = REQUIRED if args.require is None else args.require
    files = collect(args.root, args.extra)
    names = {name for name, _ in files}
    missing = [name for name in required if name not in names]
    if missing:
        sys.exit(f"release.py: {args.root} is incomplete: no {', '.join(missing)}")

    prefix = f"{args.name}-{version_of(args.version_file)}"
    out = args.outdir / f"{prefix}.zip"
    out.parent.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name, path in files:
            info = zipfile.ZipInfo(f"{prefix}/{name}", STAMP)
            info.external_attr = MODE
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, path.read_bytes())

    print(f"{out.name} {len(files)} files, {out.stat().st_size:,} bytes")


if __name__ == "__main__":
    main(sys.argv)

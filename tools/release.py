#!/usr/bin/env python3
"""Pack a built tree into a versioned archive: the site, or the SDK.

    release.py <dir> <outdir> --version-file <version.h>
               [--name <stem>] [--require <path>...] [--extra <file>...]

The archive is <stem>-<version>.zip, holding one directory of that name, so it
unpacks beside whatever else a web root already has. Entries are sorted and
share one stamp: the pack time, or SOURCE_DATE_EPOCH when it is set.
"""

import argparse
import os
import sys
import time
import zipfile
from pathlib import Path

# version.py sits beside this script, which is sys.path[0] as it is run.
from version import version_of

# What a built site cannot be missing.
REQUIRED = ("index.html", "kernel.wasm", "bundle.bin")

MODE = 0o100644 << 16

# The earliest a zip can express.
EPOCH = (1980, 1, 1, 0, 0, 0)


def stamp():
    """One timestamp for every entry: SOURCE_DATE_EPOCH, else the pack time."""
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch is None:
        # A zip's date field is local time by definition.
        t = time.localtime()
    else:
        try:
            # UTC, so a pinned stamp does not move with the packer's zone.
            t = time.gmtime(int(epoch))
        except ValueError:
            sys.exit(f"release.py: SOURCE_DATE_EPOCH is not an integer: {epoch}")
    return max(t[:6], EPOCH)


def collect(root: Path, extras):
    files = [(p.relative_to(root).as_posix(), p) for p in sorted(root.rglob("*"))
             if p.is_file()]
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

    when = stamp()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name, path in files:
            info = zipfile.ZipInfo(f"{prefix}/{name}", when)
            info.external_attr = MODE
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, path.read_bytes())

    print(f"{out.name} {len(files)} files, {out.stat().st_size:,} bytes")


if __name__ == "__main__":
    main(sys.argv)

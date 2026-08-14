#!/usr/bin/env python3
"""Pack a built site into a deployable archive.

    release.py <webdir> <outdir> --version-file <version.h> [--extra <file>...]

The archive is braam-<version>.zip, holding one directory of that name, so it
unpacks beside whatever else a web root already has. Entries are sorted and
timestamped alike, so one tree gives one archive, byte for byte.
"""

import argparse
import re
import sys
import zipfile
from pathlib import Path

# What a built site cannot be missing.
REQUIRED = ("index.html", "kernel.wasm", "bundle.bin")

# Fixed, because a zip timestamp is the difference between two identical
# builds. 1980-01-01 is the earliest a zip can express.
STAMP = (1980, 1, 1, 0, 0, 0)
MODE = 0o100644 << 16


def version_of(path: Path):
    m = re.search(r'BRAAM_VERSION\s*=\s*"([^"]+)"', path.read_text())
    if not m:
        sys.exit(f"release.py: no BRAAM_VERSION in {path}")
    return m.group(1)


def collect(root: Path, extras):
    files = [(p.relative_to(root).as_posix(), p) for p in sorted(root.rglob("*")) if p.is_file()]
    files += [(p.name, p) for p in extras]
    return sorted(files)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("webdir", type=Path)
    ap.add_argument("outdir", type=Path)
    ap.add_argument("--version-file", type=Path, required=True)
    ap.add_argument("--extra", type=Path, action="append", default=[])
    args = ap.parse_args(argv[1:])

    files = collect(args.webdir, args.extra)
    names = {name for name, _ in files}
    missing = [name for name in REQUIRED if name not in names]
    if missing:
        sys.exit(f"release.py: {args.webdir} is not a built site: no {', '.join(missing)}")

    prefix = f"braam-{version_of(args.version_file)}"
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

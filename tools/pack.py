#!/usr/bin/env python3
"""Pack a directory into the boot archive the host unpacks into OPFS.

    pack.py <dir> <out.zip>

An ordinary deflated zip, holding files only: web/fs.js creates each entry's
parent directories as it writes. `stamp` and `MODE` are here rather than in
release.py because both tools pack and only one of them is the higher level.
"""

import os
import sys
import time
import zipfile
from pathlib import Path

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
            sys.exit(f"pack.py: SOURCE_DATE_EPOCH is not an integer: {epoch}")
    return max(t[:6], EPOCH)


def collect(root: Path):
    return sorted((p.relative_to(root).as_posix(), p)
                  for p in root.rglob("*") if p.is_file())


def main(argv):
    if len(argv) != 3:
        sys.exit("usage: pack.py <dir> <out.zip>")

    root, out = Path(argv[1]), Path(argv[2])
    files = collect(root) if root.is_dir() else []
    out.parent.mkdir(parents=True, exist_ok=True)

    # Sorted entries and one shared stamp, which is what makes a pack
    # reproducible. Written whole rather than appended to, so a rebuild that
    # drops a file does not leave it in the archive.
    when = stamp()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name, path in files:
            info = zipfile.ZipInfo(name, when)
            info.external_attr = MODE
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, path.read_bytes())

    print(f"{out.name} {len(files)} files, {out.stat().st_size:,} bytes")


if __name__ == "__main__":
    main(sys.argv)

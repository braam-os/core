#!/usr/bin/env python3
"""Pack a directory into the boot archive BundleFs reads.

    pack.py <dir> <out.bin>

Layout, little-endian, as documented in src/fs/bundlefs.h:

    "BBND", u32 version, u32 count
    count * { u32 name_off, u32 name_len, u32 data_off, u32 size }
    names and data
"""

import struct
import sys
from pathlib import Path

VERSION = 1
HEADER = 12
SLOT = 16


def collect(root: Path):
    files = []
    for path in sorted(root.rglob("*")):
        if path.is_file():
            files.append((path.relative_to(root).as_posix(), path.read_bytes()))
    return files


def pack(files):
    names = [name.encode("utf-8") for name, _ in files]
    at = HEADER + SLOT * len(files)

    table, blob = bytearray(), bytearray()
    for name, (_, data) in zip(names, files):
        name_off = at + len(blob)
        blob += name
        data_off = at + len(blob)
        blob += data
        table += struct.pack("<IIII", name_off, len(name), data_off, len(data))

    return b"BBND" + struct.pack("<II", VERSION, len(files)) + bytes(table) + bytes(blob)


def main(argv):
    if len(argv) != 3:
        sys.exit("usage: pack.py <dir> <out.bin>")

    root, out = Path(argv[1]), Path(argv[2])
    files = collect(root) if root.is_dir() else []
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(pack(files))
    print(f"{out.name} {len(files)} files, {out.stat().st_size:,} bytes")


if __name__ == "__main__":
    main(sys.argv)

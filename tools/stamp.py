#!/usr/bin/env python3
"""Stamp a process binary with the metadata `exec` reads before it runs it.

    stamp.py <file.wasm> --tier N --initial-pages N --max-pages N [--sysabi <h>]

Appends a wasm custom section named "braam" holding six little-endian u32s, the
ProcMeta of src/kernel/sysabi.h:

    magic, abi, tier, flags, initial_pages, max_pages

A post-link step rather than a __attribute__((section(".custom_section.braam")))
global, because the page counts have to agree with the link's --initial-memory
and this is the one place that knows both. Any earlier "braam" section is
dropped first, so stamping twice is stamping once.
"""

import argparse
import re
import struct
import sys
from pathlib import Path

MAGIC = 0x6D617262  # "bram"
NAME = b"braam"

SYSABI = Path(__file__).resolve().parent.parent / "src" / "kernel" / "sysabi.h"


def abi(sysabi: Path) -> int:
    """PROC_ABI, read rather than restated: the number is what makes a stale
    binary a diagnostic, and a copy here that fell behind would stamp one."""
    m = re.search(r"PROC_ABI\s*=\s*(\d+)", sysabi.read_text())
    if not m:
        sys.exit(f"stamp.py: no PROC_ABI in {sysabi}")
    return int(m.group(1))


def leb128(n: int) -> bytes:
    out = bytearray()
    while True:
        byte = n & 0x7F
        n >>= 7
        if n:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def read_leb128(data: bytes, at: int):
    value, shift = 0, 0
    while True:
        byte = data[at]
        at += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value, at


def strip(data: bytes) -> bytes:
    """Everything but a "braam" custom section."""
    if data[:4] != b"\0asm":
        sys.exit("stamp.py: not a wasm module")

    out, at = bytearray(data[:8]), 8
    while at < len(data):
        start = at
        section_id = data[at]
        size, at = read_leb128(data, at + 1)
        body, at = data[at : at + size], at + size
        if section_id == 0:
            name_len, name_at = read_leb128(body, 0)
            if body[name_at : name_at + name_len] == NAME:
                continue
        out += data[start:at]
    return bytes(out)


def section(sysabi: Path, tier: int, flags: int, initial_pages: int, max_pages: int) -> bytes:
    meta = struct.pack("<IIIIII", MAGIC, abi(sysabi), tier, flags, initial_pages, max_pages)
    body = leb128(len(NAME)) + NAME + meta
    return b"\0" + leb128(len(body)) + body


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("binary", type=Path)
    # Required rather than defaulted: nothing in the system asks for tier 2 any
    # more, so a default would be the answer no caller wants.
    ap.add_argument("--tier", type=int, required=True)
    ap.add_argument("--flags", type=int, default=0)
    ap.add_argument("--initial-pages", type=int, required=True)
    ap.add_argument("--max-pages", type=int, required=True)
    # The installed SDK has no src/ tree, so it names the header it shipped.
    ap.add_argument("--sysabi", type=Path, default=SYSABI)
    args = ap.parse_args(argv[1:])

    data = strip(args.binary.read_bytes())
    args.binary.write_bytes(
        data + section(args.sysabi, args.tier, args.flags, args.initial_pages, args.max_pages)
    )


if __name__ == "__main__":
    main(sys.argv)

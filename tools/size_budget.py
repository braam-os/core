#!/usr/bin/env python3
"""Check wasm binaries against the recorded size budgets."""

import argparse
import os
import sys


def read_budgets(path):
    budgets = {}
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            if "=" not in line:
                sys.exit(f"{path}:{lineno}: expected 'name = bytes'")
            name, _, limit = line.partition("=")
            budgets[name.strip()] = int(limit.strip(), 0)
    return budgets


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--budgets", required=True)
    ap.add_argument("binaries", nargs="+")
    args = ap.parse_args()

    budgets = read_budgets(args.budgets)
    failed = False

    for path in args.binaries:
        name = os.path.basename(path)
        size = os.path.getsize(path)
        limit = budgets.get(name)
        if limit is None:
            print(f"{name}: no budget recorded in {args.budgets}", file=sys.stderr)
            failed = True
            continue
        pct = 100.0 * size / limit
        print(f"{name} {size:,} / {limit:,} bytes ({pct:.0f}%)")
        if size > limit:
            print(f"{name}: over budget by {size - limit:,} bytes", file=sys.stderr)
            failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

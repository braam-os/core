#!/usr/bin/env python3
"""Derive test/unit/solve.data from apk-tools/test/solver/.

The .repo and .installed files go in verbatim; Braam's package_read already
reads them. A .test keeps its directives and has @EXPECT rewritten: the
`(N/M)` progress prefix and the `OK:` summary are apk's printer, not its
solver, and an error block is reduced to its labels.

The scratch tree is gitignored, so this runs by hand when the corpus moves.
"""

import base64
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src/cmd/tmp/apk-tools/test/solver"
OUT = ROOT / "test/unit/solve.data"

# Every case that needs something the solver is not. The reason is carried
# into the generated file: P17 asks for the drops to be listed.
DROPPED = [
    ("repository pinning", [f"pinning{i}" for i in range(1, 16)]),
    ("apk upgrading itself", [f"selfupgrade{i}" for i in range(1, 9)]),
    ("the `fix` applet", [f"fix{i}" for i in range(1, 7)]),
    ("-t .virtual, a package built on the command line",
     ["basic8", "basic9", "error6", "error7"]),
    ("--no-network or a cache index: availability masking",
     ["basic7", "basic12", "basic16"]),
    ("-v: the verbose summary and the no-longer-available note",
     ["basic20", "installif11", "installif12"]),
    ("world dependency spelling, rejected before the solver",
     ["error8", "error9", "error10"]),
    ("--force-broken-world", ["basic10", "basic11"]),
    ("arch", ["basic19"]),
    ("an index dependency carrying @tag", ["error11"]),
    ("`apk del`'s not-removed report", ["provides-prio3"]),
]

DROP = {name: why for why, names in DROPPED for name in names}

ACTION = re.compile(r"^\(\d+/\d+\) ")
ERRFIELD = re.compile(r"^ {4}\S")
ERRWRAP = re.compile(r"^ {5,}\S")


def expectation(lines):
    """apk's output, reduced to what the solver decided."""
    out = []
    for line in lines:
        if line.startswith("OK:"):
            continue
        if ACTION.match(line):
            out.append(ACTION.sub("", line))
        elif line.startswith("ERROR: unable to select packages:"):
            out.append("ERROR:")
        elif ERRFIELD.match(line) or ERRWRAP.match(line):
            continue  # the reporter's prose, not its verdict
        elif line.strip() == "":
            continue
        else:
            out.append(line)
    return out


def restamp(value):
    """A Q1 digest as a Q2 one.

    apk's C: is SHA-1 and Package_Format.md §1.1 takes SHA-256 alone. The
    fixtures use C: only as identity, so hashing keeps equal equal and
    distinct distinct — but it must hash the *decoded* bytes: apk keys its
    package table on them, and installif1.repo spells one digest two ways
    (`...yysE=` and `...yysF=` differ only in padding bits) so that `libiif`
    collides with `bar` and never becomes a package at all. Hashing the text
    would resurrect it and break four cases.
    """
    raw = base64.b64decode(value[2:] if value[:1] == "Q" else value)
    return "Q2" + base64.b64encode(hashlib.sha256(raw).digest()).decode()


def fold_lists(text):
    """One D:, p: or i: per stanza.

    apk accumulates a repeated list letter; Package_Format.md §1 calls that
    malformed and §6 makes the list space-separated, so the two spellings mean
    the same thing and this picks the one the grammar defines.
    """
    out = []
    for stanza in text.split("\n\n"):
        lines, seen = [], {}
        for line in stanza.split("\n"):
            letter = line[:1]
            if letter == "C" and line[1:2] == ":":
                line = "C:" + restamp(line[2:])
            if letter in "Dpi" and line[1:2] == ":":
                if letter in seen:
                    lines[seen[letter]] += " " + line[2:]
                    continue
                seen[letter] = len(lines)
            lines.append(line)
        out.append("\n".join(lines))
    return "\n\n".join(out)


def convert(path):
    """A .test file, with @EXPECT rewritten. Returns (text, referenced)."""
    body, expect, seen = [], [], False
    referenced = []
    for line in path.read_text().splitlines():
        if line == "@EXPECT":
            seen = True
            continue
        if seen:
            expect.append(line)
            continue
        if line.startswith("@REPO ") or line.startswith("@INSTALLED "):
            referenced.append(line.split(" ", 1)[1].strip())
        body.append(line)
    if not seen:
        sys.exit(f"{path.name}: no @EXPECT")
    return "\n".join(body + ["@EXPECT"] + expectation(expect)) + "\n", referenced


def main():
    if not SRC.is_dir():
        sys.exit(f"{SRC} is not there; the scratch tree is gitignored")

    tests = sorted(p for p in SRC.glob("*.test"))
    kept = [p for p in tests if p.stem not in DROP]
    unknown = set(DROP) - {p.stem for p in tests}
    if unknown:
        sys.exit(f"dropped cases that do not exist: {sorted(unknown)}")

    cases, need = [], set()
    for path in kept:
        text, referenced = convert(path)
        cases.append((path.name, text))
        need.update(referenced)

    head = [
        "apk-tools/test/solver/, ported by tools/mkfixtures.py. A .repo or",
        ".installed is verbatim but for two re-spellings: C: is restamped from",
        "apk's Q1 to the Q2 §1.1 defines, hashing the old value so that equal",
        "stays equal and distinct stays distinct; and a repeated D:, p: or i:,",
        "which apk",
        "accumulates and Package_Format.md §1 calls malformed: the lines are",
        "folded into the one space-separated list §6 defines, which is the same",
        "thing said the way this grammar says it. A .test has @EXPECT reduced to",
        "the actions apk chose and the order it chose them in, the `(N/M)`",
        "counter and the `OK:` line being its printer's rather than its",
        "solver's.",
        "",
        f"{len(kept)} of {len(tests)} cases. The {len(DROP)} dropped, each needing"
        " something the solver",
        "is not:",
        "",
    ]
    for why, names in DROPPED:
        head.append(f"  {len(names):2}  {why}")
        head.append(f"      {' '.join(names)}")

    parts = ['R"DATA(', "\n".join(f"# {line}".rstrip() for line in head), ""]
    for name in sorted(need):
        parts.append(f"@FILE {name}")
        parts.append(fold_lists((SRC / name).read_text().rstrip("\n")))
    for name, text in cases:
        parts.append(f"@FILE {name}")
        parts.append(text.rstrip("\n"))
    parts.append(')DATA"')

    OUT.write_text("\n".join(parts) + "\n")
    print(f"{OUT.relative_to(ROOT)}: {len(kept)} cases, {len(need)} fixtures, "
          f"{OUT.stat().st_size} bytes")


if __name__ == "__main__":
    main()

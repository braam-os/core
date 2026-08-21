# /bin/pkg — development tasks

A sequential plan. `/bin/pkg` is a skeleton;
[doc/Package_Management.md](../../../doc/Package_Management.md) is the policy
written before it, and this is the order the code goes in.

Read that document first. Where it and this file disagree, it wins — and where
either disagrees with [doc/Concept.md](../../../doc/Concept.md), Concept.md
wins. A bare `§N` below is a section of Package_Management.md.

Alpine's `apk` is the model for the resolver, the index and the version
grammar; Debian's `apt` for the command names; Nix for activation. The sources
are in `src/cmd/tmp/apk-tools/` (gitignored scratch, not part of the tree).

**A finished task is deleted from this file, and the numbers do not move** —
Release_Notes.md and the tasks below cite them. Phases A and B are gone that
way: the decisions are in Concept.md, Package_Management.md and
Package_Format.md, and the two host operations are in System_Calls.md. Phase C
went with them, P5 to P12: `src/cmd/pkg/` is a directory beside `src/cmd/sh/`,
`braam_pkg` is the library its `main.cpp` links, `pkg.cpp` holds the subcommand
table each task below fills a row of, and `sha256.cpp`, `encode.cpp`,
`version.cpp`, `dep.cpp`, `stanza.cpp`, `zip.cpp`, `db.cpp`, `trust.cpp`,
`index.cpp`, `plan.cpp` and `trigger.cpp` are the digest, the two encodings,
apk's version grammar, the dependency token, the stanza grammar with its
records, the zip's directory, `/pkg`'s layout, the anchor's checks, §7's
pipeline, what a changeset means and which triggers a transaction wakes,
compiled into `tests.wasm` as well — `trust.cpp` and `index.cpp` because they
take a `PkgHost` rather than calling a syscall.
`unzip.cpp` inflates an entry, `store.cpp` performs the steps `db.cpp`
computes, `host.cpp` is the `PkgHost` `/bin/pkg` uses, and `update.cpp`,
`query.cpp`, `verify.cpp`, `clean.cpp` and `install.cpp` are the subcommands
there are — the last holding the four that change the installed set — and
`script.cpp` spawns §5.1's scripts; those nine are the pieces that stay out, and
`solve.cpp` is in with the first list, being pure. Phases D and E are done too:
every row of the table is a command, `pkg update` writes `/pkg/index`,
`pkg search`, `pkg info` and `pkg list` read it and the generation beside it,
`pkg install`, `pkg remove`, `pkg autoremove` and `pkg upgrade` perform the
solver's changeset, `pkg files` and `pkg verify` read §8.1's record back, and
`pkg clean` collects what none of them names any more. P23 went the way Phases A
and B did: `cmd:` was never code here, and the grammar it asked for is
Package_Format.md §6.1. Phase F is done as well: P24 ran §5.1's six scripts
around the commit, and P25 fired `.trigger` after the whole transaction, both
under the paragraph §11 wrote before either — nothing implies a fence §11 says
does not exist, and a package that only places files still runs no code. So this
starts at P26.

Four of P26's tools came early with P18, which needed a signed repository to
install from: `tools/ed25519.py`, `signindex.py`, `mkanchor.py`, `mkpkg.py`,
`mkindex.py` and the fixture driver `mkrepo.py`, which writes
`test/unit/repo.data` under keys it destroys.

## The shape being built

The commands, all of them:

| Command | Purpose |
| --- | --- |
| `pkg update` | Download and check the repository index |
| `pkg search <pattern>` | Search available packages |
| `pkg info <package>` | Show what the index says about one package |
| `pkg install <package>...` | Install packages and their dependencies |
| `pkg remove <package>...` | Remove packages |
| `pkg autoremove` | Remove dependencies nothing explicit needs any more |
| `pkg upgrade` | Upgrade everything installed |
| `pkg list` | List installed packages |
| `pkg files <package>` | List an installed package's files |
| `pkg verify [<package>]` | Check installed files against their digests |
| `pkg clean` | Drop downloaded archives and unreferenced generations |

### The `/pkg` tree

`/pkg` is a new top-level directory, and the archive does **not** carry it —
which is what §11 asks of `pkg`'s record, since the unpack replaces every
top-level directory the archive *does* carry.

Defined in [doc/Package_Format.md](../../../doc/Package_Format.md) §8; in
outline:

```
/pkg/repositories              one URL per line; today, one line
/pkg/index                     the last checked index, signature block and all
/pkg/store/<name>-<version>/   unpacked, checked, immutable once written
/pkg/db/<name>-<version>       the installed-db stanza: per-file digests
/pkg/gen/<N>/packages          a generation: the whole installed set, as text
/pkg/gen/<N>/bin/<cmd>         a symlink into /pkg/store — the generation's PATH
/pkg/active                    a symlink to gen/<N> — the commit point
/pkg/bin                       a symlink to active/bin — what PATH names
/pkg/world                     the explicitly-installed set (apk's world file)
/pkg/cache/                    downloaded archives; `pkg clean` empties this
```

**A generation is a directory**, holding both the text and the links that make
it runnable, so one rename commits the two together.

An install unpacks and checks into `/pkg/store/`, builds `/pkg/gen/<N+1>/` whole
including its `bin/` links, and then swings `/pkg/active` — write
`/pkg/active.new` and `Sys::Rename` it over `/pkg/active`. **That single rename
is the commit.** A tab that dies before it leaves rubbish in `/pkg/store` that
`pkg clean` collects; a tab that dies after it has installed. Rolling back is
swinging the link back.

`/pkg/bin` never moves, so nothing has to be told a generation changed.

### Command resolution

A command word resolves as function, then builtin, then `PATH` (Concept.md §4),
and **no clause is added to any of the three**. An installed program is reached
the way every other program is: `/pkg/bin` is on the default search list after
`/bin`, so `/bin` still wins and nothing installed can shadow the system. The
kernel learns none of `/pkg`'s file formats, and the whole of activation is a
symlink and one word of `SYS_PATH_DEFAULT`, which is built.

The alternative, which was the plan before symbolic links, `Sys::Rename` and
`PATH` landed, was a fourth clause in `exec_resolve` reading `/pkg/active` and
`/pkg/gen/<N>` on every miss. Release_Notes.md holds what it would have cost.

### What the host already provides

`Sys::Verify` (Ed25519) and `Sys::Inflate` (raw deflate, answering with a
descriptor) are built; System_Calls.md §8 is what they carry, and §9 lists
`Inflate`'s handle kind.

SHA-256 is **not** among them and never will be: it is compiled into `pkg`. A
`crypto.subtle.digest` is one-shot, so a host-side digest would mean staging a
whole package through `SYS_STAGE_MAX` and capping a package at a megabyte. In
wasm it hashes the body as it streams off the fetch descriptor, and nothing
large crosses the ABI. That is P6.

## Phase G — the other side of the wire, and proof

### P26. Publisher tools

The five that build and sign came early, with P18. What is left:

- `tools/mkindex.py` — Package_Format.md §6.1's `cmd:` names, scanned out of
  each package's `bin/` rather than copied from its `.PKGINFO`. The rule is
  written; nothing produces it. `tools/mkrepo.py` gives up its hand-written
  `p:cmd:hi` in the same change, so `test/unit/repo.data` proves the derivation
  instead of restating it, and `test/run.mjs` gains the case §6.1's version
  clause exists for: `pkg install cmd:hi` selects a provider.
- A real `rootfs/share/pkg/anchor`, signed by keys somebody holds, replacing
  the placeholder P13 shipped, whose private halves were destroyed the moment
  it was signed. `tools/mkanchor.py` is what signs it.

§9 is absolute and this is where it is at risk. **No private key** may be in the
git tree, in anything built from it, inside `rootfs.zip`, or anywhere a browser
can reach. The signer reads a key from a path given on its command line, keeps
nothing, and writes nothing but the signature. `pkg` itself never signs and
holds no code to make a key.

Create a section in doc/Package_Format.md where explain for unsophisticated
reader, in a tutorial style, how to use the publisher tools to build and
maintain a file server with binary packages.

### P27. End to end

A static repository fixture served by `test/fakesvc.mjs`, and `test/run.mjs`
cases for the happy path and for every attack §3 names:

- a tampered package — the hash does not match;
- a wrong signature;
- a signature by a key the anchor does not name;
- one key's signature repeated to meet a threshold;
- an expired index;
- an index whose version went backwards;
- a name the index does not list;
- a body longer than the size the index gave;
- a tab that dies between the store write and the `/pkg/active` rename.

Each must be a refusal that says which check failed. A test that only proves the
happy path proves nothing here.

### P28. Documentation

`rootfs/README` and `rootfs/share/help`.

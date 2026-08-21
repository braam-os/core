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
`index.cpp` and `plan.cpp` are the digest, the two encodings, apk's version
grammar, the dependency token, the stanza grammar with its records, the zip's
directory, `/pkg`'s layout, the anchor's checks, §7's pipeline and what a
changeset means, compiled into `tests.wasm` as well — `trust.cpp` and
`index.cpp` because they take a `PkgHost` rather than calling a syscall.
`unzip.cpp` inflates an entry, `store.cpp` performs the steps `db.cpp`
computes, `host.cpp` is the `PkgHost` `/bin/pkg` uses, and `update.cpp`,
`query.cpp` and `install.cpp` are the subcommands there are; those six are the
pieces that stay out, and `solve.cpp` is in with the first list, being pure.
Phase D is done, and of Phase E `pkg update` writes `/pkg/index`, `pkg search`,
`pkg info` and `pkg list` read it and the generation beside it, and
`pkg install` performs the solver's changeset. So this starts at P19.

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

## Phase E — the commands

### P19. `pkg remove` / `pkg autoremove`

`/pkg/world` is the explicitly-installed set. `remove` takes a name out of it
and re-solves; `autoremove` drops what is no longer reachable from it. Both
commit a new generation the same way P18 does.

### P20. `pkg upgrade`

Re-solve `/pkg/world` against the current index, and commit one generation for
the lot. Not a loop of installs: §6's "the whole package set, signed as one
file" is what stops packages that never went together being combined, and one
transaction is what preserves it.

### P21. `pkg files` / `pkg verify`

`files` prints the paths from the installed-db stanza. `verify` re-hashes each
recorded file against its stored digest and reports missing, modified and extra;
with no argument it does every installed package.

Say plainly in the help text what this does not mean. §11: checking happens
once, at install, nothing re-reads the store afterwards, and anything can
overwrite anything — including `/bin/pkg`. `verify` tells you a file changed.
It does not make the store tamper-evident.

### P22. `pkg clean`

The download cache, store directories no generation names, and superseded
generations. **Keep at least the previous generation**: it is what rollback
needs, and a clean that removes it removes the property P18 was built for.

---

## Phase F — scripts and triggers

§11 permits install scripts and says what one is: an
ordinary `/bin/sh` process with the authority of whoever typed `pkg install`,
because there is no privilege boundary here to give it less. Build to that
paragraph — in particular, nothing below is allowed to imply a fence §11 says
does not exist, and a package that only places files must still run no code at
all.

### P23. `cmd:`

A provides namespace, generated by the publisher tool from the executables a
package carries, and consumed by the solver as an ordinary name. apk does not
special-case it anywhere and neither should this — `cmd:awk` is a name whose
providers happen to be the packages that ship an `awk`.

### P24. Install scripts

`pre-install`, `post-install`, `pre-deinstall`, `post-deinstall`,
`pre-upgrade`, `post-upgrade` — run as `/bin/sh` through `Sys::Spawn`, in the
changeset's order, with apk's argv convention: the new version, and on an
upgrade the old one after it.

A failing script marks the package broken and is recorded rather than aborting.
That is apk's behaviour and it is what gives `pkg verify` something to find.

### P25. Triggers

A package's `.trigger` script and its path globs. Fired once per package after
the whole transaction, with the matched directories as argv. A freshly
installed package runs all of its triggers; an existing one runs a trigger whose
glob matches a directory the transaction modified.

---

## Phase G — the other side of the wire, and proof

### P26. Publisher tools

The five that build and sign came early, with P18. What is left:

- `tools/mkindex.py` — the `cmd:` auto-provides scanned out of each package,
  which is P23's grammar and not yet written down.
- A real `rootfs/share/pkg/anchor`, signed by keys somebody holds, replacing
  the placeholder P13 shipped, whose private halves were destroyed the moment
  it was signed. `tools/mkanchor.py` is what signs it.

§9 is absolute and this is where it is at risk. **No private key** may be in the
git tree, in anything built from it, inside `rootfs.zip`, or anywhere a browser
can reach. The signer reads a key from a path given on its command line, keeps
nothing, and writes nothing but the signature. `pkg` itself never signs and
holds no code to make a key.

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

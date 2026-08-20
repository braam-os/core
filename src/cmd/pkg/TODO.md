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
`version.cpp`, `dep.cpp`, `stanza.cpp`, `zip.cpp`, `db.cpp`, `trust.cpp` and
`index.cpp` are the digest, the two encodings, apk's version grammar, the
dependency token, the stanza grammar with its records, the zip's directory,
`/pkg`'s layout, the anchor's checks and §7's pipeline, compiled into
`tests.wasm` as well — the last two because they take a `PkgHost` rather than
calling a syscall. `unzip.cpp` inflates an entry, `store.cpp` performs the steps
`db.cpp` computes, `host.cpp` is the `PkgHost` `/bin/pkg` uses and `update.cpp`
is the first subcommand; those four are the pieces that stay out. Phase D is
done and `pkg update` is built, so there is a checked index in `/pkg/index` and
nothing yet that reads it. So this starts at P16.

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

### P16. `pkg search` / `pkg info` / `pkg list`

Read-only, over `/pkg/index` as `pkg update` left it and the local db. Cheap,
and they are what makes an update's result inspectable — which is worth having
before the solver lands.

`search` matches the name and the description; `info` prints one package's
stanza in a readable shape; `list` reads the generation, not the index. The
stored file is read the way it was written, `signed_split` and `header_read`
over the whole of it, since a stanza is a view into the text.

### P17. The solver

`solve.cpp` — apk's algorithm, which is greedy, deductive and has **no
backtracking**. Once a name is locked it is not revisited; a contradiction is
accumulated and reported rather than retried.

- The model: a **name** is any dependency token, real or virtual, holding its
  providers and its reverse dependencies; a **package**; a **provider** is a
  (package, version) pair registered under a name, where the version is the one
  it provides *under that name*.
- Three work queues, each sorted by a discovery order: resolve-now (a name with
  no options left — unit propagation), selectable, unresolved.
- `apply_constraint` counts requirers and disqualifies every provider that
  cannot satisfy; `disqualify_package` re-dirties the reverse dependents, which
  is the up-propagation.
- `reconsider_name` is the propagation core: re-check each candidate's depends,
  re-evaluate `install_if`, merge the dependencies common to *all* candidates so
  a constraint can be applied before the choice is made, and exclude
  non-providers where every candidate also provides some other name.
- `compare_providers` is a strict tiebreak chain. Several of apk's rungs go with
  repository pinning and `so:`; what is left is roughly: fewer conflicts, then
  installed, then higher version, then higher provider priority.
- An unversioned virtual provider is never chosen spontaneously unless it has a
  provider priority or its own package name is required.
- The changeset comes out in **dependency order**, because the generator
  recurses into a package's depends before recording it.

`apk-tools/test/solver/` is 168 files — 119 `.test` cases over 29 `.repo` and 20
`.installed` files, the last two in the stanza grammar and the first a list of
`@ARGS`, `@REPO`, `@INSTALLED`, `@WORLD` and `@EXPECT` directives, where
`@EXPECT` is the expected output compared byte for byte. Port the harness; the
fixtures are the specification of this task.

Done when: the ported fixtures pass, minus the ones that only exercise pinning
or `so:`, and the ones dropped are listed with a reason.

### P18. `pkg install <package>...`

§7's steps 8 to 10, then activation.

1. Fetch each package **capped at the exact size the index gave**.
2. Stream it through SHA-256 while writing it to `/pkg/cache/`.
3. Compare hash **and** size against the index.
4. Only now unzip, into `/pkg/store/<name>-<version>/`.
5. Write `/pkg/gen/<N+1>/packages`, and materialise its `bin/` link farm.
6. Write `/pkg/active.new` and `Sys::Rename` it over `/pkg/active`.

Step 3 to step 4 is the rule's only crossing point, and it should read that way
in the code.

Record the index version that vouched for each package (§7's last paragraph), so
a reinstall re-checks rather than believing the disk.

Done when: an install works, a hash mismatch leaves nothing behind, and killing
the tab between steps 5 and 6 leaves the old generation active — the link never
having moved.

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

- `tools/mkpkg.py` — a package zip, reproducible: reuse `pack.py`'s `stamp()`
  and `MODE` rather than restating them.
- `tools/mkindex.py` — the index, including the `cmd:` auto-provides scanned
  out of each package.
- `tools/signindex.py` — Ed25519, and the `Y:` block Package_Format.md §2
  defines. It must compute the signed region the way `pkg` does, which is the
  one place a publisher and a client can disagree in silence.
- `tools/mkanchor.py` — the anchor, signed by a threshold of root keys, and
  what replaces the placeholder `rootfs/share/pkg/anchor` P13 shipped, whose
  private halves were destroyed the moment it was signed.

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

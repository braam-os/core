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
Package_Format.md, and the two host operations are in System_Calls.md. P5 to P10
went with them: `src/cmd/pkg/` is a directory beside `src/cmd/sh/`,
`braam_pkg` is the library its `main.cpp` links, `pkg.cpp` holds the subcommand
table each task below fills a row of, and `sha256.cpp`, `encode.cpp`,
`version.cpp`, `dep.cpp`, `stanza.cpp` and `zip.cpp` are the digest, the two
encodings, apk's version grammar, the dependency token, the stanza grammar with
its records and the zip's directory, compiled into `tests.wasm` as well.
`unzip.cpp` is the half of the zip reader that inflates, and is the one piece
that stays out. So this starts at P11.

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
symlink and one word of `SYS_PATH_DEFAULT` — see P12.

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

---

## Phase C — pkg's own primitives, no network

### P11. The local store

`db.cpp`, over Package_Format.md §8: read and write `/pkg/gen/<N>/packages` and
its `bin/` link farm, `/pkg/world`, `/pkg/repositories` and the installed-db
stanzas, and read `/pkg/active` — which is a symlink, so `read_link` and not
`Read`.

The recursive `mkdir` this needs now exists: `make_dir_all` (`src/proc/io.h`),
the walk over the components tolerating `Error::Exists`, since `Sys::MkDir` is
one level and refuses an existing directory (`vfs_mkdir`, `src/fs/vfs.cpp`).
`/bin/mkdir -p` is its other caller. Build a store directory with it rather than
a chain of `make_dir` calls.

`rename_path` (`src/proc/io.h`, over `Sys::Rename`) does exist. Its
`Err(Unsupported)` means "copy instead" — a directory, or a move across mounts —
and `/bin/mv`'s `move_one` and `copy_tree` (`src/cmd/mv.cpp`) are the worked
example, including how they recreate a symlink rather than following it.
`Sys::Remove` has a recursive bit, which is what drops a store directory.

Done when: a generation written and read back is identical, and a `/pkg` tree
built from nothing has the right shape.

### P12. Activation, by symlink

`exec_resolve` is not touched. The whole of the kernel half is one word:
`SYS_PATH_DEFAULT` (`src/kernel/sysabi.h`) becomes `/bin:/pkg/bin`, and init
plants the same thing (`src/user/boot.cpp`). `/bin` first, so nothing installed
shadows the system; `/pkg/bin` is a symlink to `active/bin` and `active` a
symlink to the live generation, so a `pkg install` changes what a command word
finds without anything being told.

It degrades quietly for free, and that is the point of doing it this way. A
missing `/pkg`, a dangling `/pkg/active`, a farm entry pointing at nothing and a
name no generation lists are all a `PATH` component that finds nothing — already
`Err(NotFound)` and 127, already tested, and no new failure path in the kernel
to get wrong. A component that is not a directory is skipped for the same
reason (`exec.cpp` continues on `NotFound`, `NotDir` and `IsDir`).

The one thing to check is that `PATH` is a **default** and not a floor: a
process spawned with `PATH=/x` searches `/x` alone, installed programs included,
which is what a `PATH` that steers resolution has to mean.

Done when: `test/run.mjs` runs a binary reached through a hand-built
`/pkg/store`, `/pkg/gen/1/bin` link farm and `/pkg/active` symlink; `/bin` still
wins for a name in both; and a missing `/pkg`, a dangling `/pkg/active` and a
farm entry pointing at nothing each give 127. Also that `/pkg` survives a
version change, which is `web/fs.js`'s unpack naming only `bin` and `share`.

---

## Phase D — trust

Everything above can be got wrong and fixed. This cannot: §7's rule is that
nothing is unzipped, written or run before its hash matches a hash from a
signed index, and the rule has exactly one crossing point.

### P13. The anchor

`rootfs/share/pkg/anchor`, unpacked to `/share/pkg/` at boot with the rest of
the archive, and the loader for it.

- A key is named by the SHA-256 of its public key. A name that is a hash of the
  key cannot be claimed by a different key.
- Threshold counting takes **at most one signature per key**. Otherwise one
  signature repeated meets any threshold — §7 step 4 says this outright, and it
  is the single easiest thing here to get wrong.
- Missing or unreadable: stop. There is no fallback and nothing to rebuild it
  from.
- The anchor-chain walk of §10: an anchor is signed by a threshold of the old
  root keys *and* a threshold of the new ones, and anchors are numbered, so a
  client at anchor 1 walks forward to anchor 3 checking each against the one
  before it.

There is **no prompt** (§6). A key becomes trusted by shipping in the archive,
or by a person typing its full fingerprint. There is no trust-on-first-use.

Done when: a good anchor loads; an anchor one signature short of the threshold
is refused; an anchor meeting the threshold with one key's signature repeated is
refused; an anchor 3 is reached from anchor 1 and is refused if anchor 2 is
withheld.

### P14. The checked-index pipeline

§7's steps 1 to 7, in that order and in one function, so that the order is
reviewable in one screen:

1. **Fix the time once** from `Sys::Clock` and use that one value everywhere. A
   clock that moves mid-run must not make two checks disagree.
2. **Load the anchor.**
3. **Fetch the index, capped — and well below `SYS_STAGE_MAX`.** `Sys::Verify`
   takes the signed bytes as one staged payload, so an index over a megabyte
   cannot be checked at all; the cap has to leave room for that. `web/svc.js`
   imposes no size limit on a body, so
   this is `pkg`'s to enforce: count the bytes coming off the descriptor and
   close it when the cap is passed. Longer than the cap is a failure, not a
   truncation.
4. **Check the signatures** to the anchor's threshold.
5. **Check the version** against the highest seen before. Lower is a rollback
   and a failure; equal means nothing to do and is not an error.
6. **Check the expiry** against the time from step 1.
7. **Only now read the index.** A name it does not list does not exist and is
   not looked for anywhere else.

Every failure abandons the whole operation and reports which step failed. There
is no `--force`, no `--insecure` and no `--no-verify`, in any form or spelling.

Done when: each of the six failures is a distinct, tested refusal.

---

## Phase E — the commands

### P15. `pkg update`

Fetch, check, record. The whole of Phase D behind one word, and the first thing
a user can run.

### P16. `pkg search` / `pkg info` / `pkg list`

Read-only, over the stored index and the local db. Cheap, and they are what
makes P15's result inspectable — which is worth having before the solver lands.

`search` matches the name and the description; `info` prints one package's
stanza in a readable shape; `list` reads the generation, not the index.

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
- `tools/mkanchor.py` — the anchor, signed by a threshold of root keys.

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

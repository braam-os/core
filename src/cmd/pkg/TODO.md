# /bin/pkg — development tasks

A sequential plan. `/bin/pkg` does not exist;
[doc/Package_Management.md](../../../doc/Package_Management.md) is the policy
written before it, and this is the order the code goes in.

Read that document first. Where it and this file disagree, it wins — and where
either disagrees with [doc/Concept.md](../../../doc/Concept.md), Concept.md
wins. A bare `§N` below is a section of Package_Management.md.

Alpine's `apk` is the model for the resolver, the index and the version
grammar; Debian's `apt` for the command names; Nix for activation. The sources
are in `src/cmd/tmp/apk-tools/` (gitignored scratch, not part of the tree).

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
which is what §11 asks of `pkg`'s record, since the unpack deletes every
top-level directory the archive *does* carry.

```
/pkg/store/<name>-<version>/   unpacked, checked, immutable once written
/pkg/gen/<N>                   a generation: the whole installed set, as text
/pkg/gen/<N>/bin/<cmd>         a symlink into /pkg/store — the generation's PATH
/pkg/active                    a symlink to gen/<N> — the commit point
/pkg/bin                       a symlink to active/bin — what PATH names
/pkg/world                     the explicitly-installed set (apk's world file)
/pkg/db/<name>-<version>       the installed-db stanza: per-file digests
/pkg/index/<repo>              the last checked index, and its signature
/pkg/cache/                    downloaded archives; `pkg clean` empties this
```

An install unpacks and checks into `/pkg/store/`, builds `/pkg/gen/<N+1>` whole
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

### Two new operations

- **`Sys::Verify`** — Ed25519 through `crypto.subtle.verify`, which Concept.md
  §6 already reserves.
- **`Sys::Inflate`** — `DecompressionStream("deflate-raw")`. The payload is the
  compressed entry and the status is a descriptor, so `Read` and `Close` serve
  it exactly as they serve a fetched body and nothing is duplicated.

SHA-256 is **not** one of them: it is compiled into `pkg`. A
`crypto.subtle.digest` is one-shot, so a host-side digest would mean staging a
whole package through `SYS_STAGE_MAX` and capping a package at a megabyte. In
wasm it hashes the body as it streams off the fetch descriptor, and nothing
large crosses the ABI.

Both are described in `doc/System_Calls.md` §8 already, at 57 and 58, as rows
that no enum yet carries (P1). P3 and P4 make them true.

---

## Phase A — the decisions, on paper first

### P1. Amend the documents — **done**

Nothing is built until these say what is being built. Three things were settled;
the rest of this file assumes them.

- **Activation is symlinks on `PATH`, not a fourth resolution clause.**
  Concept.md §4 says so, §5.1 gains `/pkg` and the fact that the archive does
  not carry it, and §5.2's unpack sentence names `/pkg` beside `/home`. P12 is
  rewritten around it.
- **SHA-256 is compiled into `pkg`; the signature check is a host service.**
  Concept.md §6's digest bullet is split into three: a `verify` bullet, a
  paragraph on why there is no digest operation, and an `inflate` bullet.
- **Install scripts run, and nothing fences them.** Package_Management.md §11 is
  rewritten: there is no privilege boundary here, so what is written down is
  what a script *is* — an ordinary `/bin/sh` process with the user's whole
  authority — and what a signature therefore authorises. Phase F is unblocked.
  §11 also now says `/pkg` survives a version change, which is what its "a
  directory the archive does not carry" sentence was pointing at.

`doc/System_Calls.md` carries `Verify` and `Inflate` at 57 and 58 as **reserved
rows**, with a paragraph each and a line saying they are in no enum yet.
`PROC_ABI` stays 14 in every document until P3 and P4 move it. Release_Notes.md
holds the *why* for all of it.

### P2. Freeze the formats

A section of `doc/Package_Management.md`, still not code. §7 settles what the
index must *contain* and explicitly leaves the format to `pkg`'s design; this
is that design, written down before anything parses it.

- **The index** — APKINDEX: blank-line-separated stanzas of `K:value`, one
  letter and a colon. A header stanza carrying the index version, the expiry
  and the repository; then one stanza per package. Field letters kept:
  `P` name, `V` version, `C` checksum, `S` size, `I` installed size,
  `T` description, `D` depends, `p` provides, `i` install-if, `o` origin,
  `t` build time, `k` provider priority.
- **The anchor** — `/share/pkg/anchor`: the root public keys, the threshold,
  the index keys the root currently vouches for, the anchor's own number and
  expiry, and an algorithm name on every key (§8).
- **A signature** — signify-shaped: two base64 lines, a comment and the
  signature, no keyring format and no PKI. A key is named by the SHA-256 of its
  public key (§8), never by a filename.
- **A package** — a zip, and what its entries mean: where the files go, and
  where the scripts and the trigger globs live.
- **The installed db and the generation** — `/pkg/db/<name>-<version>`'s
  stanza, including per-file digests, and `/pkg/gen/<N>`'s grammar.

---

## Phase B — the ABI

Both of these are enum values on each side (Concept.md §2.2), not new imports.
`SvcOp`'s values are positional and restated by hand in `web/svc.js`, so
**append at the end of the enum, never insert**.

### P3. `Sys::Verify` / `SvcOp::Verify`

Ed25519 over `crypto.subtle.verify`. The payload is the public key, the
signature and the signed bytes; the reply is a yes or a no, and an error is not
a no.

Touches: `src/svc/svc.h`, a new `src/svc/crypto.cpp` and its `CMakeLists.txt`
line, `src/kernel/sysabi.h` (`Sys::Verify = 57`, and **bump `PROC_ABI` to 15**),
`src/user/syscall.cpp`, `src/proc/io.h` and `io.cpp`, `web/svc.js` (the `OP`
table and a `perform` arm), `test/fakesvc.mjs` (whose switch fails closed on an
op it does not know), `test/unit/`.

`doc/System_Calls.md`'s row for it already exists (P1) and says what the payload
carries; this is where it stops being reserved. Drop it from the reserved note
under the table, move `PROC_ABI` in the ABI-history paragraph and at `:262`, and
correct Concept.md's count sentence.

§8 is load-bearing here: where the browser has no Ed25519 the operation answers
`Err(Unsupported)` and `pkg` **refuses to run**. It must be impossible for that
path to become a skipped check.

Done when: a unit test verifies a known-good RFC 8032 vector, rejects a
tampered one, and rejects a signature by the wrong key.

### P4. `Sys::Inflate` / `SvcOp::Inflate`

Payload in, descriptor out. Needs a `Handle::Kind::Inflate` in
`src/user/proctab.h` with its arms in `shut()`, in `Sys::Read` and in
`Sys::Close`, and a row in `doc/System_Calls.md` §9's handle-kind table.
`Sys::Inflate = 58`, **`PROC_ABI` to 16**, and the reserved note under the
async table goes with it.

The compressed input is capped at `SYS_STAGE_MAX` (1 MiB) because it is one
staged payload; the output is not capped. A compressed entry larger than that
is refused, and the bound is documented rather than worked around.

Same file set as P3.

Done when: a unit test round-trips a deflate stream produced by `tools/pack.py`,
and a truncated stream is an error rather than a short read.

---

## Phase C — pkg's own primitives, no network

### P5. Skeleton

`src/cmd/pkg/` on the shell's pattern — it is the only other program with a
directory: a `CMakeLists.txt` building a `braam_pkg` static library, and a
`main.cpp` holding `proc_main` that stays out of the library.

Touches: `src/cmd/pkg/CMakeLists.txt`, `src/cmd/pkg/main.cpp`,
`src/cmd/CMakeLists.txt` (`add_subdirectory(pkg)`, `pkg` in `BRAAM_BIN_LIST`,
`BRAAM_BIN_LIB_pkg`, `BRAAM_BIN_SRC_pkg`), `rootfs/share/help`, the help list
in `test/run.mjs`.

Nothing else in the test harness needs touching: the per-binary import
assertion is a loop over the binaries CMake hands it.

Done when: `pkg` with no subcommand prints usage and exits 2, and `make run`
passes — which is the smoke test asserting that `pkg` imports nothing but the
process ABI.

### P6. SHA-256, and the encodings

`sha256.h`/`sha256.cpp`: an init/update/final API, so a body is hashed as it
arrives rather than held whole. Plus hex and base64, encode and decode — the
tree has neither, and `src/kernel/hash.h` is a HashMap, not a digest.

Done when: the NIST vectors pass, including the multi-block and empty-input
cases, and a digest computed in one update matches one computed a byte at a
time.

### P7. Version comparison

apk's grammar, in `version.cpp`:

```
digit{.digit}...{letter}{_suf{#}}...{~hash}{-r#}
```

What has to be right, because it is where the subtlety is:

- The **token-type ordering** is the semantics. When the two sides diverge in
  token *type*, the side whose next token is a pre-release suffix is the lesser
  one — that is what makes `1.1_alpha1 < 1.1`.
- The **suffix table** with `NONE` as the pivot:
  `alpha beta pre rc <none> cvs svn git hg p`.
- A digit run beginning with `0` compares **as a string**, not as a number.
- Fuzzy (`~`) is: if the right side ran out, the result is equal. That single
  rule is the whole of prefix matching.

`apk-tools/test/unit/version.data` is 788 comparison cases. Port it into
`test/unit/` as data rather than restating it — a rewritten table is a table
with new mistakes in it.

Done when: all 788 cases pass.

### P8. Dependency parsing

`[!]name[[op]ver]`, with apk's result-mask model: comparison yields exactly one
of EQUAL, LESS or GREATER, and the operator is the *mask of acceptable
results*. Then `=`, `<`, `>`, `<=`, `>=`, `~`, `>~`, `<~` and a leading `!`
are one `match()` and a bitfield, not nine cases.

Two of apk's namespaces do not come across. **`so:` is dropped** — it exists for
ELF shared libraries and every binary here is statically linked and
self-contained. **`cmd:` stays**, as an ordinary name (P23). The `@tag`
repository-pinning suffix and the `><` checksum operator go too: there is one
repository, and the index already names a package by its hash.

Done when: a table-driven test covers each operator, the conflict form, an
unparseable version (which marks the dependency broken rather than failing the
file), and a dependency list split on both spaces and newlines.

### P9. APKINDEX

The stanza reader and writer. The rules that matter:

- A line is one letter, a colon, then the value. A blank line ends the stanza
  and commits the package.
- An unknown **uppercase** letter marks the package uninstallable — fail
  closed.
- An unknown **lowercase** letter is ignored — forward compatibility.

That asymmetry is apk's and it is worth keeping: it is what lets a future index
carry a field this `pkg` has never heard of without either lying about a
package or refusing the whole file.

Done when: a round trip through the writer and reader is byte-identical, a
stanza with an unknown uppercase field yields an uninstallable package, and a
file with no trailing blank line still commits its last stanza.

### P10. Zip reader

`zip.cpp`, over `Sys::Inflate`. Mirror `web/fs.js`'s `parseZip` rule for rule,
because two readers of one format that disagree is how a package installs
differently from the way it was signed:

- Scan backwards for the end-of-central-directory record.
- Walk the central directory, and **re-read each local header to find where the
  data begins**. Taking the central directory's offset for the data is the
  classic way to get this wrong.
- Zip64 refused. Encrypted refused. Methods 0 and 8 only.
- A name check refusing absolute paths, backslashes, drive letters and any `.`
  or `..` component.
- The CRC-32 is stepped past. §7 says why: a CRC is not a security check, and
  whoever chooses the bytes chooses a matching CRC. The hash from the signed
  index is the check.

Done when: it reads `rootfs.zip` itself and produces the same entries
`web/fs.js` does.

### P11. The local store

`db.cpp`: read and write `/pkg/gen/<N>` and its `bin/` link farm, `/pkg/world`
and the installed-db stanzas, and read `/pkg/active` — which is a symlink, so
`read_link` and not `Read`.

One helper has to be written because the system does not have it. There is **no
recursive `mkdir`** — `Sys::MkDir` is one level and refuses an existing
directory (`vfs_mkdir`, `src/fs/vfs.cpp`), so this is a walk over the components
tolerating `Error::Exists`, the way `web/fs.js`'s `installOps` does it;
`boot.cpp`'s `make_dirs` is a fixed list and not a helper, and `mkdir` has no
`-p`.

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
3. **Fetch the index, capped.** `web/svc.js` imposes no size limit on a body, so
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

`apk-tools/test/solver/` is 168 fixtures in plain APKINDEX text with `@ARGS`,
`@REPO` and `@EXPECT` stanzas. Port the harness; the fixtures are the
specification of this task.

Done when: the ported fixtures pass, minus the ones that only exercise pinning
or `so:`, and the ones dropped are listed with a reason.

### P18. `pkg install <package>...`

§7's steps 8 to 10, then activation.

1. Fetch each package **capped at the exact size the index gave**.
2. Stream it through SHA-256 while writing it to `/pkg/cache/`.
3. Compare hash **and** size against the index.
4. Only now unzip, into `/pkg/store/<name>-<version>/`.
5. Write `/pkg/gen/<N+1>` whole, and materialise its `bin/` link farm.
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

**Unblocked by P1.** §11 permits install scripts and says what one is: an
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
- `tools/mkindex.py` — the APKINDEX, including the `cmd:` auto-provides scanned
  out of each package.
- `tools/signindex.py` — Ed25519, signify-shaped.
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

### P28. Budget and documentation

Measure `pkg` against `tools/size_budget.txt`. The staging tree has room today,
but SHA-256, a zip reader, a solver and an index parser are not free; raising
the `rootfs/` line is a deliberate act and needs a Release_Notes.md entry saying
what bought the bytes.

Then `rootfs/README` and `rootfs/share/help`.

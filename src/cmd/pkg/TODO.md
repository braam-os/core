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
/pkg/active                    one line, "<N>" — the commit point
/pkg/world                     the explicitly-installed set (apk's world file)
/pkg/db/<name>-<version>       the installed-db stanza: per-file digests
/pkg/index/<repo>              the last checked index, and its signature
/pkg/cache/                    downloaded archives; `pkg clean` empties this
```

An install unpacks and checks into `/pkg/store/`, writes `/pkg/gen/<N+1>`
whole, and then writes `/pkg/active`. **That single write is the commit.** A
tab that dies before it leaves rubbish in `/pkg/store` that `pkg clean`
collects; a tab that dies after it has installed. Rolling back is writing the
old number back.

### Command resolution

A command word resolves as function, then builtin, then `/bin` (Concept.md §4).
A fourth clause is added after `/bin`: read `/pkg/active`, read `/pkg/gen/<N>`,
take the line naming that command. Two small reads on a `/bin` miss, and
deliberately not cached — the reason the prompt's cwd is not cached either.
`/bin` still wins, so nothing installed can shadow the system.

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

---

## Phase A — the decisions, on paper first

### P1. Amend the documents

Nothing is built until these say what is being built.

- **`doc/Package_Management.md` §11** — the paragraph "No install scripts, on
  purpose" forbids what P24 builds. Rewrite it, and answer the question it
  poses: what may a script touch? Note that there is no privilege boundary here
  (§11's first paragraph), so the honest statement is what a script *is*, not
  what it is prevented from doing.
- **`doc/Concept.md` §4** — the fourth clause of command resolution. **§5.1** —
  `/pkg`, and that the archive does not carry it. **§6** — an `inflate` bullet,
  and the digest bullet corrected: SHA-256 moved into wasm, and why.
- **`doc/System_Calls.md`** — the two new operation-table rows, and the
  `PROC_ABI` line at `:262`.
- **`doc/Release_Notes.md`** — a new appended heading holding the *why* for all
  of it.

While there: `Concept.md:691` says "the table is thirty-eight operations and
`PROC_ABI` is 10". Both are already wrong.

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
line, `src/kernel/sysabi.h` (`Sys::Verify = 57`, and **bump `PROC_ABI` to 13**),
`src/user/syscall.cpp`, `src/proc/io.h` and `io.cpp`, `web/svc.js` (the `OP`
table and a `perform` arm), `test/fakesvc.mjs` (whose switch fails closed on an
op it does not know), `test/unit/`.

§8 is load-bearing here: where the browser has no Ed25519 the operation answers
`Err(Unsupported)` and `pkg` **refuses to run**. It must be impossible for that
path to become a skipped check.

Done when: a unit test verifies a known-good RFC 8032 vector, rejects a
tampered one, and rejects a signature by the wrong key.

### P4. `Sys::Inflate` / `SvcOp::Inflate`

Payload in, descriptor out. Needs a `Handle::Kind::Inflate` in
`src/user/proctab.h` with its arms in `shut()`, in `Sys::Read` and in
`Sys::Close`. `Sys::Inflate = 58`.

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

`db.cpp`: read and write `/pkg/gen/<N>`, `/pkg/active`, `/pkg/world` and the
installed-db stanzas.

Two helpers have to be written because the system does not have them. There is
**no recursive `mkdir`** — `Sys::MkDir` is one level and refuses an existing
directory, so this is a walk over the components tolerating `Error::Exists`,
the way `web/fs.js`'s `installOps` and `boot.cpp`'s `make_dirs` each do it. And
there is **no `rename`**, so moving a file is a copy and a remove. `Sys::Remove`
does have a recursive bit, which is what drops a store directory.

Done when: a generation written and read back is identical, and a `/pkg` tree
built from nothing has the right shape.

### P12. The `exec_resolve` search path

`src/user/exec.cpp` — the fourth clause, after `/bin`. This is the kernel half
of activation and it is small: read `/pkg/active`, read `/pkg/gen/<N>`, take the
line naming the command, resolve the binary under `/pkg/store/`.

It must degrade quietly. A missing `/pkg`, a missing `/pkg/active`, a
generation file that does not parse, or a name the generation does not list are
all "command not found" — never an error, and never a boot that will not
finish.

Done when: `test/run.mjs` runs a binary placed by hand in a fabricated
`/pkg/store` with a hand-written generation; `/bin` still wins for a name in
both; and each of the four degraded cases gives 127.

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
5. Write `/pkg/gen/<N+1>` whole.
6. Write `/pkg/active`.

Step 3 to step 4 is the rule's only crossing point, and it should read that way
in the code.

Record the index version that vouched for each package (§7's last paragraph), so
a reinstall re-checks rather than believing the disk.

Done when: an install works, a hash mismatch leaves nothing behind, and killing
the tab between steps 5 and 6 leaves the old generation active.

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

**Blocked on P1.** §11 forbids install scripts today, and the argument it makes
is not weak: a package that runs code at install time is a package whose
signature authorises arbitrary execution rather than file contents. Do not start
this phase until that paragraph has been rewritten with an answer.

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
- a tab that dies between the store write and the `/pkg/active` write.

Each must be a refusal that says which check failed. A test that only proves the
happy path proves nothing here.

### P28. Budget and documentation

Measure `pkg` against `tools/size_budget.txt`. The staging tree has room today,
but SHA-256, a zip reader, a solver and an index parser are not free; raising
the `rootfs/` line is a deliberate act and needs a Release_Notes.md entry saying
what bought the bytes.

Then `rootfs/README` and `rootfs/share/help`.

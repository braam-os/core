# Braam — Package formats

The five files `/bin/pkg` reads and writes: a signature, the index, the anchor,
a package, and the local state under `/pkg`.

[Package_Management.md](Package_Management.md) is the policy and settles what
the index must *contain*; this is the grammar. Where they disagree the policy
wins, and where either disagrees with [Concept.md](Concept.md) the specification
wins. A bare `§N` is a section of this document.
[Release_Notes.md](Release_Notes.md) holds the reasoning.

Alpine's `apk` is the model. Its field letters and version grammar are kept
unchanged, so `test/unit/version.data`'s 788 cases and `test/solver/`'s 119
fixtures port as data. §9 lists every departure.

Where each part lives:

| Part | Code |
| --- | --- |
| §1, §1.1, the records | [src/cmd/pkg/stanza.h](../src/cmd/pkg/stanza.h), `stanza.cpp` |
| §1.1's encodings | [src/cmd/pkg/encode.h](../src/cmd/pkg/encode.h) |
| §2, §3, §4 | `index.cpp`, [src/cmd/pkg/trust.cpp](../src/cmd/pkg/trust.cpp) |
| §5, §5.2 | [src/cmd/pkg/zip.h](../src/cmd/pkg/zip.h), `unzip.cpp`, and `parseZip` in [web/fs.js](../web/fs.js) |
| §5.1, §5.1.1 | [src/cmd/pkg/install.cpp](../src/cmd/pkg/install.cpp), `script.cpp`, `trigger.cpp` |
| §6, §7 | [src/cmd/pkg/dep.cpp](../src/cmd/pkg/dep.cpp), `version.cpp`, `solve.cpp` |
| §8 | [src/cmd/pkg/db.h](../src/cmd/pkg/db.h), `store.cpp` |
| §10's tools | `tools/ed25519.py`, `mkanchor.py`, `mkpkg.py`, `mkindex.py` |

---

## 1. The stanza grammar

One grammar, one reader, five files.

```
P:awk
T:pattern-directed scanning and processing language
```

A **line** is one letter, a colon, and a value running to end of line. No space
after the colon; a value may contain spaces. A **stanza** is a run of lines; an
**empty line ends it, and so does end of file**. A file is a sequence of
stanzas.

- **An unknown uppercase letter makes the record unusable** — that record, not
  the file.
- **An unknown lowercase letter is ignored.** A field that is merely
  informational must therefore be lowercase from the day it is added.
- **A repeated letter is malformed**, except `Y`, `K`, `H`, `F`, `R` and `Z`,
  which accumulate.
- **A letter means one thing in every file.** `G` is the version of a signed
  document, `E` an expiry, `T` a description, wherever they appear.
- **A known letter whose value does not parse, or a required field that is
  absent, makes the record unusable** — the same scope as an unknown uppercase
  letter.

### 1.1 Numbers, digests and keys

A number is decimal, unsigned, unpadded: `007`, `+1`, `1a` and an overflow are
all refused. A time is **milliseconds since the epoch** — what `Sys::Clock`
reports and `Sys::Stat` returns.

A digest is apk's `<encoding><algorithm><payload>`:

```
Q2IgfM18bBUW8blv5C1wE491Z5bfWNc+VRhcgcX1hLHUI=
││└──────────────── base64 of 32 bytes, standard alphabet, padded
│└───────────────── algorithm: 2 = SHA-256
└────────────────── encoding: Q = base64
```

**`Q2` is the only accepted form** (Package_Management.md §8).

Base64 decoding is **strict**: a length that is a multiple of four, `=` only as
the last one or two characters, no character outside the alphabet, and the
unused bits of a short final group zero. Two spellings must not decode alike.

A **public key** is `<algorithm> <base64 key>`. A **key's name** is the `Q2`
digest of its public key, and is never stored beside the key it names.

---

## 2. A signature

The first stanza of a signed file, one line per signature:

```
Y:ed25519 Q2IgfM18bBUW8blv5C1wE491Z5bfWNc+VRhcgcX1hLHUI= <base64 signature>
```

Algorithm, the signing key's name, the signature.

> **The signed bytes are every byte of the file after the first empty line.**

- **A key's name is matched by recomputing it**, never trusted as a label.
- A `Y:` naming a key the anchor does not carry counts for nothing; it is not an
  error.
- **Threshold counting takes at most one signature per key** —
  Package_Management.md §7 step 4.

---

## 3. The index

A plain text file: signature block, empty line, header stanza, empty line, then
one stanza per package. **The header is the first stanza after the signature
block**, by position.

```
Y:ed25519 <keyid> <signature>

X:1
N:https://packages.example/braam
G:41
E:1755648000000
T:Braam packages

C:Q2...
P:awk
V:1.2-r0
S:18244
I:41984
T:pattern-directed scanning and processing language
D:cmd:sh
p:cmd:awk=1.2-r0

C:Q2...
P:less
...
```

### 3.1 The header stanza

| Letter | Value | |
| --- | --- | --- |
| `X` | grammar version, currently `1` | required |
| `N` | the repository's URL, no trailing slash | required |
| `G` | index version, decimal, only ever increasing | required |
| `E` | expiry | required |
| `T` | description, for a human | optional |

- `X` names *this grammar*, not the index. **A higher `X` refuses the whole
  file** — the one place fail-closed applies to a file rather than a record.
- **`N` must equal the URL the index was fetched from**, or the index is
  refused.
- `G` and `E` are checked at Package_Management.md §7's steps 5 and 6, after the
  signatures and never before. **`X` and `N` are checked when the header is
  read**, between step 4 and step 5.

### 3.2 A package stanza

| Letter | Value | |
| --- | --- | --- |
| `C` | digest of the package zip | required |
| `P` | name | required |
| `V` | version (§7) | required |
| `S` | the zip's exact size in bytes | required |
| `I` | unpacked size; caps the unpack when below 16 MiB (§5.1) | optional |
| `T` | description | optional |
| `D` | depends — a dependency list (§6) | optional |
| `p` | provides — a dependency list, and §6.1's generated names | optional |
| `i` | install-if — a dependency list | optional |
| `o` | origin — the source package's name | optional |
| `t` | build time | optional |
| `k` | provider priority | optional |
| `g` | trigger globs, space-separated (§5.1.1) | optional |

`C` and `S` are what Package_Management.md §7's steps 8 and 9 check against.

**`P` and `V` are path components.** `/pkg/store/<P>-<V>/` and `/pkg/db/<P>-<V>`
are built out of them (§8), so neither may be empty, be `.` or `..`, hold a `/`
or a `\`, or hold a byte below `0x20`; a stanza whose `P` or `V` is not one is
unusable. A colon may appear in either — `cmd:awk` is an ordinary §6 name and
nothing in a path. The rule is the reader's, so it holds for the index, for
`.PKGINFO` (§5.1) and for §8.1's record alike, which is what stops a sideloaded
`.PKGINFO` naming a directory of its own choosing (Package_Management.md §7.1).

apk's `A` (arch) is dropped — there is one architecture. Its `U`, `L`, `m` and
`c` are undefined here: the two lowercase ones are ignored, the two uppercase
ones make a package unusable.

### 3.3 Canonical order, and the package's URL

A writer emits `C P V S I T o t k g D p i`, omitting what it has not got, so
that a round trip is byte-identical. **A reader requires no particular order.**

The index is at `<N>/index` and a package at `<N>/<name>-<version>.zip`.
**Derived, never carried**: a URL proves nothing (Package_Management.md §4), so
no field names one.

---

## 4. The anchor

`/etc/anchor`, shipped in `rootfs.zip` and re-pinned from it at every version
change (Package_Management.md §6). Signature block, empty line, one stanza.

```
Y:ed25519 <keyid> <signature>
Y:ed25519 <keyid> <signature>

X:1
G:3
E:1787184000000
H:root 2
H:index 1
K:root ed25519 <base64 public key>
K:root ed25519 <base64 public key>
K:root ed25519 <base64 public key>
K:index ed25519 <base64 public key>
```

| Letter | Value | |
| --- | --- | --- |
| `X` | grammar version, currently `1` | required |
| `G` | the anchor's version, decimal, only ever increasing | required |
| `E` | expiry | required |
| `H` | `<use> <count>` — a threshold, repeats | once per use |
| `K` | `<use> <algorithm> <base64 key>`, repeats | required |

`use` is `root` or `index`; any other is ignored, so a third role needs no
grammar version.

- **Missing or unreadable is a stop.** There is no fallback and no prompt
  (Package_Management.md §6).
- **A higher `X` refuses the whole file**, as it does for an index (§3.1).
- **`E` is checked against the caller's fixed time** (Package_Management.md §7
  step 1). An expired anchor is refused whatever else it says.
- **Every anchor meets its own `H:root` over its own `K:root`** — the one in the
  archive as much as one walked to. An anchor amended by hand after signing is
  refused.
- **An `H` comes once per use and a `K` once.**
- **A `K` of another algorithm is left alone**; an `ed25519` one whose key is
  not 32 bytes makes the anchor unusable.
- **The chain walk is `G`.** A client at anchor 1 reaches anchor 3 by checking 2
  against 1 and 3 against 2 (Package_Management.md §10). `G` must increase and
  need not increase by one. Withholding 2 stops the walk; it does not let 3
  through.

---

## 5. A package

A zip — `web/fs.js` reads one and `DecompressionStream` inflates one already.

### 5.1 Entries

**A top-level entry whose name begins with `.` is metadata; everything else is
payload**, unpacked into `/pkg/store/<name>-<version>/`. Only a name with no `/`
can be metadata, so `bin/.keep` is an ordinary file.

| Entry | |
| --- | --- |
| `.PKGINFO` | the package's own stanza, §3.2's grammar |
| `.pre-install` `.post-install` | `/bin/sh` scripts |
| `.pre-deinstall` `.post-deinstall` | `/bin/sh` scripts |
| `.pre-upgrade` `.post-upgrade` | `/bin/sh` scripts |
| `.trigger` | a `/bin/sh` script, for the globs `g:` names (§5.1.1) |

- **An unknown top-level dot-entry makes the package uninstallable** — §1's
  uppercase rule applied to an entry name.
- **`.PKGINFO` is required.** It carries §3.2's letters **less `C` and `S`**,
  which name the archive and cannot be inside it, so a reader takes it field by
  field rather than as a whole §3.2 stanza — and **carrying either is a
  refusal**, since it would be a claim about bytes the reader is holding.
- **What `.PKGINFO` authorises depends on what named the archive.** For a
  package from a repository it authorises nothing and the index does: **`P` and
  `V` must agree with the index stanza** that vouched for it, and nothing else
  is compared. For one named outright (Package_Management.md §7.1) there is no
  index stanza and **`.PKGINFO` is the stanza**: `C` and `S` are computed from
  the archive, §6.1's `cmd:` names are derived from its flat `bin/`, and §3.2's
  path-component rule on `P` and `V` is what bounds where it can write.
- **A dot-entry other than `.PKGINFO` is kept**, written into
  `/pkg/store/<name>-<version>/` under its own name and recorded in §8.1's file
  list like any payload file — `.pre-deinstall` runs at a removal, when the
  archive is gone. `.PKGINFO` is not kept; the record supersedes it.
- **The unpack is capped** at `I` when it is set and below 16 MiB, otherwise at
  16 MiB, summed over the kept entries and applied to each one.

Scripts run as Package_Management.md §11 describes, with apk's argv convention:
the new version, and on an upgrade the old one after it. A removal passes the
version leaving and nothing after it. Each is spawned as `/bin/sh <file>`, so
the file need carry no `#!`.

**The commit is the line between `pre-` and `post-`.** Every `pre-` script runs
after each package is fetched, checked and unpacked and before §8.3's rename;
every `post-` script runs after it. A `post-` script can run what was just
installed and a `pre-` script cannot.

A script that fails is recorded (§8.1's `b`) and the transaction carries on
(Package_Management.md §11). A trigger is a script like the six.

### 5.1.1 `.trigger`, and what wakes it

`.trigger` takes **directories** as argv rather than versions, and runs **once
per package, after the whole transaction** — after the `post-` scripts, which
are themselves after the commit.

A transaction has a view of two sets of directories. The **modified** set is
what it wrote: each unpacked package's store directory and each of its `F`s,
plus **`/pkg/bin`**, whose contents change even when a removal writes nothing to
the store. The rest is every directory of every package the transaction leaves
installed.

For each installed package carrying `g:`, and each directory:

- Skip the directory unless the package is **fresh** — installed or upgraded by
  this transaction — or the directory was modified. A purged package's triggers
  do not fire at all.
- Take the globs in order. A leading `+` is stripped and means **only-changed**.
  A glob not then starting with `/` is skipped. Matching is **per component**:
  `*` does not cross a `/`, so `/pkg/store/*/share` names one package's `share`
  and not everything beneath the store.
- The **first** glob that matches settles that directory: the trigger will now
  fire, and the directory joins argv unless only-changed and the directory was
  not modified.

Two consequences. A `+` glob matching an unmodified directory **still wakes the
trigger** — it only withholds that directory, so a fresh package whose globs are
all `+` runs with an empty argv. And a package whose globs match nothing does
not run its trigger at all.

### 5.2 What the reader accepts

`parseZip`'s rules (`web/fs.js`), written down once so the two readers can be
checked against each other.

- Scan backwards for the end-of-central-directory record, within a 64 KiB
  comment window.
- **Refuse zip64** — an entry count of `0xffff` or a directory offset of
  `0xffffffff`.
- **Refuse an encrypted entry** — flag bit 0.
- Skip a name ending in `/`; the paths imply their directories.
- **Refuse a name** that is empty, absolute, holds a backslash, begins with a
  drive letter, or has a `.` or `..` component.
- **Re-read the local header to find where the data begins**, never the central
  directory's offset.
- Methods **0 (store) and 8 (deflate)** only.
- **Step past the CRC-32** (Package_Management.md §7). The digest from the
  signed index is the check, taken over the whole zip before an entry is read.

**The order above is normative.** The `/` test runs before the name test, so
`../` is *skipped* and not refused; the method is judged after the local header
has been found. A reader that reorders them refuses archives the other accepts.

Two rules a reader given a stream needs and one given a buffer does not:

- **Stop at the entry's declared uncompressed size**, and refuse a stream that
  ends before it or runs past it. The declared size is inside the digested
  archive and is as trusted as the archive; the inflated bytes are not.
- **An entry compressed larger than `SYS_STAGE_MAX`** cannot be read at all:
  `Sys::Inflate` stages its input (System_Calls.md §8). `rootfs.zip`'s largest
  entry is 75 KB compressed.

`parseZip` checks neither: `DecompressionStream` hands back a buffer, and the
one archive it reads is the release's own.

---

## 6. Dependencies

```
[!]name[[op]ver]
```

A leading `!` is a conflict. The operator is the maximal run of `< > = ~`; the
rest is a version. **The operator is a mask of acceptable results**, so nine
spellings are one `match()` and a bitfield:

| Character | Bits contributed |
| --- | --- |
| `<` | LESS |
| `>` | GREATER |
| `=` | EQUAL |
| `~` | EQUAL, FUZZY |

A comparison yields exactly one of EQUAL, LESS, GREATER; a dependency matches
when that bit is in the mask, inverted by `!`. So no operator is any version,
`=` `<` `>` `<=` `>=` read as they look, and `~` `<~` `>~` are fuzzy (§7).

A list is **space- or newline-separated**, runs of separators collapsing.

**A name is any token**, and need not be a package: `cmd:awk` is an ordinary
name whose providers ship an `awk` (§6.1). apk's `so:` is dropped. A *package's
own* name is narrower — §3.2's `P` is a path component — but a dependency may
name anything, since nothing builds a path out of one.

**An unparseable version marks the dependency broken, not the file** — the
stanza becomes an uninstallable package and every other stanza still reads. A
broken dependency names something and is satisfied by nothing.

**A token with no name, or an operator with nothing after it, is malformed** —
`=1.2`, `foo>=`, a bare `!`. That is a field which is not a dependency list, and
the reader refuses the record over it.

### 6.1 `cmd:` names

The one generated namespace. A package provides **`cmd:<command>=<V>` for every
entry of its `bin/`**, `<V>` being the package's own version: `hello-1.0-r0`
shipping `bin/hi` provides `cmd:hi=1.0-r0`.

- **`bin/`, and flat.** That is exactly the set §8.3's link farm carries — one
  link per entry, directories skipped — so `cmd:x` holds precisely when `x` on
  `PATH` runs this package's file. `bin/sub/tool` yields nothing.
- **Whether the entry is a program (Concept.md §4) is not asked.** The farm does
  not ask either.
- **The version is what makes the name selectable.** A name whose providers are
  all unversioned can be depended on but never installed: it is virtual. With
  one, `pkg install cmd:awk` picks a provider and `cmd:awk>=1.2` compares the
  providing package's version.
- **The publisher generates them, into the index stanza.** A package need not
  declare them; `.PKGINFO` need carry none. A `p:` line written by hand is an
  ordinary provide and merges with them. `/pkg/db` is written from the index
  stanza (§8.1), so a solve against the installed set sees what a solve against
  the index saw. **An archive named outright has no index stanza, so `pkg`
  derives them itself** from the same flat `bin/` (Package_Management.md §7.1)
  — the invariant is the point, not who computed it.
- **Two packages shipping one command both provide one name, and that is not a
  conflict.** Both may be installed, and §8.3's "whichever the farm wrote last"
  decides which runs. A dependency on `cmd:x` is satisfied by either, with `k`
  (§3.2) choosing. Making co-installation impossible is a package's own `!`
  conflict to declare.
- **Nothing special-cases the prefix.** §6's reader, the index lookup and the
  solver see a name that happens to contain a colon.

---

## 7. Versions

apk's grammar, unchanged:

```
digit{.digit}...{letter}{_suf{#}}...{~hash}{-r#}
```

`1.2`, `2.0b`, `1.1_alpha1`, `0.9_git20240101`, `1.4~a3f91c`, `1.2-r3`.

- **The token-type ordering is the semantics.** In order: initial digit, digit,
  letter, suffix, suffix number, commit hash, revision number, end. Where two
  versions diverge in token *type*, the side whose next token is a pre-release
  suffix is the lesser — which is what makes `1.1_alpha1 < 1.1`.
- **The suffix table, `none` the pivot:**
  `alpha beta pre rc <none> cvs svn git hg p`. Left of the pivot sorts below the
  bare version, right of it above.
- **A digit run beginning with `0` compares as a string**, so `1.07 < 1.1`.
- **Fuzzy (`~`) is one rule:** if the right side runs out, the result is equal.
  That is the whole of prefix matching.

---

## 8. The local state

Under `/pkg`, which the archive does not carry (Concept.md §5.1) — bar the one
line of configuration, which sits with the anchor in `/etc` and is re-pinned by
a release like it (Package_Management.md §6).

```
/etc/repositories                one URL per line; today, one line
/pkg/index                       the last checked index, signature and all
/pkg/store/<name>-<version>/     unpacked, checked, immutable once written
/pkg/db/<name>-<version>         what was installed, and what vouched for it
/pkg/gen/<N>/packages            a generation: the installed set
/pkg/gen/<N>/bin/<cmd>           a symlink into /pkg/store
/pkg/active                      a symlink to gen/<N> — the commit point
/pkg/bin                         a symlink to active/bin — what PATH names
/pkg/world                       the explicitly-installed set
/pkg/cache/<name>-<version>.zip  a downloaded zip; `pkg clean` empties this
```

The cache leaf is §3.3's URL leaf, so a cached archive is named by what it is
rather than by where it came from. It is **re-hashed against the index every
time it is used** and never believed for being on disk.

**A generation is a directory**, holding the text and the links together, so one
`Sys::Rename` of `/pkg/active` commits both.

### 8.1 The installed database

`/pkg/db/<name>-<version>`: §3.2's stanza as the index gave it, plus

| Letter | Value | |
| --- | --- | --- |
| `G` | the index version that vouched, or `0` for none | required |
| `b` | the install script that failed (§5.1), without its dot | optional |
| `F` | a directory, relative to the store directory, repeats | |
| `R` | a filename under the last `F`, repeats | |
| `Z` | the digest of the file the last `R` named | |

A writer emits §3.3's order, then `G`, then `b`, then each `F` followed by its
`R` and `Z` pairs, so the round trip is defined here too.

The file list covers §5.1's kept dot-entries as well as the payload, so a script
at the top of the package is an `F` of `""` and an `R` of `.post-install`.

`b` is **lowercase on purpose**: a reader that does not know it ignores it and
loses a warning rather than the record (§1).

`G` is what makes a reinstall re-check rather than believe the disk
(Package_Management.md §7). **`G: 0` is a package no index vouched for** — one
named outright (§7.1), whose stanza came out of its own `.PKGINFO`. It is `0`
for the *bytes* and not for the operand, so an archive carried in by hand whose
digest the index does list records that index's `G` like any other. `pkg verify`
reports a `G: 0` record as `unvouched` without failing over it, and `pkg info`
reads `vouched no`.

apk's `M:` and `a:` are dropped: they carry uid, gid and mode, and there are
none here.

`pkg verify` re-reads each `R` and compares against its `Z`. What that does not
mean is Package_Management.md §11.

### 8.2 A generation, and world

`/pkg/gen/<N>/packages`, one line per package, sorted by name — positional like
`/proc/tasks`, two fields, both required. The store directory is
`<name>-<version>` by construction and is not written down.

```
awk 1.2-r0
less 1.6-r1
```

`/pkg/world` is one dependency (§6) per line: what the user asked for, as
distinct from what was pulled in to satisfy it.

`/etc/repositories` is one URL per line, and **`pkg` refuses a second line
rather than ignoring it**: `/pkg/index` is one file and
Package_Management.md §7 step 5's floor is one number. A trailing slash is
stripped, `<N>/index` being `//index` otherwise.

In all three a blank line is skipped and a last line without a newline is still
a line; **a file that is not there reads as an empty one**, so a `/pkg` that has
never been written to needs no seeding, and emptying `/etc/repositories` points
the system at nothing. There are no comments.

### 8.3 Committing a generation

Building generation `N` is, in order: remove `/pkg/gen/<N>` and make it again,
write its `packages`, make its `bin/` and fill it, write `/pkg/active.new` as a
symlink to `/pkg/gen/<N>`, and **rename that over `/pkg/active`**. Only the last
step is visible to anything else, which makes it the commit; a tab that dies
before it leaves a generation directory nothing names.

Every link is written as an absolute path — `/pkg/bin` to `/pkg/active/bin`,
`/pkg/active` to `/pkg/gen/<N>`, and each farm entry to
`/pkg/store/<name>-<version>/bin/<cmd>`. A *reader* of `/pkg/active` takes the
target as written and accepts either spelling.

Two packages shipping one command leaves whichever the farm wrote last. Making
that impossible is the solver's, not this layer's.

---

## 9. What is deliberately not apk's

| Here | apk | |
| --- | --- | --- |
| `Q2` digests only | `Q1`, `X1`, `X2`, a promoted `Q1` | one algorithm, no negotiation |
| the index is text | `APKINDEX.tar.gz` | no tar reader for one member |
| a header stanza with `G` and `E` | neither; a client-side mtime | the policy requires both |
| end of file commits a stanza | a last stanza with no blank line is dropped | no silent loss |
| an empty line ends a stanza | any line under two bytes does | a one-character line is malformed |
| a letter means one thing | letters are reused between files | one reader, five files |
| no `><` | a checksum comparison operator | the index names a package by hash |
| no `@tag` | repository pinning | there is one repository |
| no `so:` | an ELF shared-library namespace | every binary here is static |

---

## 10. Building a package repository

The publisher's procedure. Everything above is what `pkg` reads; this is how to
write it.

A repository is **a directory of static files behind any web server** — no
server-side code, no database, no upload API. Given
`N = https://packages.example/braam`, the server holds

```
braam/index                     the signed index
braam/hello-1.0-r0.zip          one file per package
braam/libz-1.0-r0.zip
```

and nothing else. §3.3 derives both URLs from `N`.

The six tools are in `tools/`, all Python 3. The ones that sign need
`pip3 install cryptography`; nothing else does.

### Step 1 — make four keys

```
python3 tools/ed25519.py root1.key root2.key root3.key index.key
```

Each line printed is a path, the public key and the key's `Q2…` id. **Three root
keys and one index key** (Package_Management.md §5):

- a **root key** signs anchors and nothing else. Make it on a machine that has
  never served the repository, encrypt it with a passphrase kept elsewhere, back
  it up, and copy its key id onto paper. Three keys, three holders, two needed.
- the **index key** signs the index and nothing else, and lives on the machine
  that publishes.

`ed25519.py` refuses to write over a key that exists. Nothing else in the tree
reads a key, and `pkg` never signs.

### Step 2 — sign an anchor

The anchor (§4) names those public keys and the thresholds over them. It is the
one file that is **not** downloaded: it ships inside `rootfs.zip`.

```
python3 tools/mkanchor.py --out anchor --version 1 --expiry 1861920000000 \
    --threshold root=2 --threshold index=1 \
    --key root=root1.key --key root=root2.key --key root=root3.key \
    --key index=index.key \
    --sign root1.key --sign root2.key
```

`--key` names private halves and writes down their public ones; `--sign` names
the private halves that sign. Two signatures, because `--threshold root=2` says
so — an anchor must meet its own root threshold. `--expiry` is milliseconds
since the epoch:

```
python3 -c 'import datetime as d; print(int(d.datetime(2029,1,1,
    tzinfo=d.timezone.utc).timestamp()) * 1000)'
```

Copy the result to `rootfs/etc/anchor`, put your repository's URL in
`rootfs/etc/repositories` beside it, and rebuild. Then put the root keys back
where they came from; publishing does not need them again until a key is rotated
or the anchor's expiry comes round, and each new anchor carries a higher
`--version`.

### Step 3 — build packages

A package is a zip (§5): payload entries, and a `.PKGINFO` written for you.

```
python3 tools/mkpkg.py --out hello-1.0-r0.zip --name hello --version 1.0-r0 \
    --field T='a greeting' --field D=libz \
    build/bin/hi=bin/hi \
    greeting.txt=share/hello/greeting
```

Each trailing `<src>=<entry>` puts a local file at that path inside the package.
Only `--name` and `--version` are required; `--field <L>=<value>` sets any other
letter of §3.2, `D` and `T` being the two worth setting. Versions are apk's
grammar (§7), so `-r0` is the release number and `1.0-r1` supersedes `1.0-r0`.

Three conventions do work for you:

- **`bin/` is what lands on `PATH`.** Every flat entry becomes a link in the
  installed generation's `bin/` (§8.3) and a `cmd:<name>` provide (§6.1).
- **A dot-entry is metadata.** `.pre-install`, `.post-install` and the four
  others are `/bin/sh` scripts run around the commit; `.trigger` runs when a
  directory your `g:` globs name changes (§5.1).
- **The zip is reproducible.** Same inputs, same bytes.

**Try it before you sign anything.** Get the zip into the store — `fimport`, or
`curl` from wherever you built it — and install it by name:

```
pkg install /import/hello-1.0-r0.zip
```

`pkg` says `unverified: no index vouches for it`, which is the truth: nothing
has been signed yet. Everything else runs as it will after publishing — the
`.PKGINFO` is read, the scripts fire, `bin/` reaches `PATH`, the triggers wake —
so a broken `D:` or a `.post-install` that exits non-zero is found here rather
than by whoever installs it first. `pkg remove hello` puts it back.

### Step 4 — sign an index

One index over every package the repository offers:

```
python3 tools/mkindex.py --out index --url https://packages.example/braam \
    --version 41 --expiry 1790000000000 \
    --description 'Example packages' --sign index.key \
    hello-1.0-r0.zip libz-1.0-r0.zip
```

It reads each zip: `C` and `S` from the bytes, the rest from `.PKGINFO`, and
§6.1's `cmd:` names from `bin/`. **`--version` must increase** at every
publication — a client refuses an index older than the one it holds (§3.1) — and
`--expiry` is a promise to re-sign before that moment. A month is normal.

Every package listed must sit in the same directory as `index` on the server.
**A package the index does not list is never fetched from the repository** — a
name it does not carry does not exist (Package_Management.md §7 step 7) — though
somebody at a keyboard may still install the archive by naming it outright, and
is told that nothing vouched for it when they do (§7.1).

### Step 5 — copy it up, and try it

Upload `index` and the zips together. On the client:

```
echo https://packages.example/braam > /etc/repositories
pkg update
pkg install hello
```

`pkg update` checks, in this order (Package_Management.md §7): the anchor's
expiry, the index's signature against the anchor's index keys, the index's
expiry, and its `G` against the one already held. `pkg install` then checks each
package's size and digest against the index stanza that vouched for it. A
refusal names the step it stopped at:

| It says | You |
| --- | --- |
| the anchor is refused | shipped an anchor that expired, or edited one by hand |
| the signature does not verify | signed with a key the anchor does not name |
| the index is older | forgot to raise `--version` |
| not in the index | rebuilt a package without rebuilding the index |
| the digest does not match | uploaded a zip and an index from different builds |

### Keeping it

- **Adding or updating a package**: build the zip, re-run `mkindex.py` over the
  whole set with `--version` raised, upload both. There is no incremental
  update; the index is one signed file.
- **Before the expiry**: re-run the same command with a later `--expiry`. That
  needs only the index key.
- **Rotating the index key**: make a new one, sign a new anchor naming it with a
  higher `--version`, and ship that anchor in a release. The old index key stops
  being trusted the moment clients take the new anchor.
- **A stolen root key**: below the threshold, sign a new anchor without it. At
  or above it, the anchor has to be replaced out of band — which is a release,
  and the key ids on paper are what let anyone check the new one.

`tools/mkrepo.py` does all five steps in forty lines to build the test fixture,
under keys it destroys afterwards.

### What never leaves your machine

Package_Management.md §9. **No private key** goes into the git tree, into
anything built from it, or inside `rootfs.zip`. The signing tools read a key
from a path, keep nothing, and write nothing but the signature.

# Braam — Package formats

The five files `/bin/pkg` reads and writes: a signature, the index, the anchor,
a package, and the local state under `/pkg`.

[Package_Management.md](Package_Management.md) is the policy and settles what
the index must *contain*; this is the grammar. Where they disagree the policy
wins, and where either disagrees with [Concept.md](Concept.md) the specification
wins. A bare `§N` is a section of this document.
[Release_Notes.md](Release_Notes.md) holds the arguments.

Alpine's `apk` is the model, and `src/cmd/tmp/apk-tools/` (gitignored scratch)
is the reference. Its field letters and version grammar are kept unchanged
because `test/unit/version.data`'s 788 cases and `test/solver/`'s 119 fixtures
port as *data* only while they are. §9 lists every departure.

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
- **An unknown lowercase letter is ignored.** This asymmetry is the whole of
  forward compatibility. Its corollary: a field that is merely informational
  must be lowercase from the day it is added.
- **A repeated letter is malformed**, except `Y`, `K`, `H`, `F`, `R` and `Z`,
  which accumulate. Nothing silently overwrites.
- **A letter means one thing in every file.** `G` is the version of a signed
  document, `E` an expiry, `T` a description, wherever they appear.
- **A known letter whose value does not parse, or a required field that is
  absent, makes the record unusable** — the same scope as an unknown uppercase
  letter, because the consequence is the same and the file still reads.

### 1.1 Numbers, digests and keys

A number is decimal, unsigned, unpadded. A time is **milliseconds since the
epoch** — what `Sys::Clock` reports and `Sys::Stat` returns.

A digest is apk's `<encoding><algorithm><payload>`:

```
Q2IgfM18bBUW8blv5C1wE491Z5bfWNc+VRhcgcX1hLHUI=
││└──────────────── base64 of 32 bytes, standard alphabet, padded
│└───────────────── algorithm: 2 = SHA-256
└────────────────── encoding: Q = base64
```

**`Q2` is the only accepted form** (Package_Management.md §8). The `2` is the
algorithm name §8 requires, in one character.

A **public key** is `<algorithm> <base64 key>`. A **key's name** is the `Q2`
digest of its public key, and is never stored beside the key it names — a
derivable name cannot disagree with what it names.

---

## 2. A signature

The first stanza of a signed file, one line per signature:

```
Y:ed25519 Q2IgfM18bBUW8blv5C1wE491Z5bfWNc+VRhcgcX1hLHUI= <base64 signature>
```

Algorithm, the signing key's name, the signature.

> **The signed bytes are every byte of the file after the first empty line.**

One rule, computed identically by signer and checker.

- **A key's name is matched by recomputing it**, never trusted as a label.
- A `Y:` naming a key the anchor does not carry counts for nothing; it is not an
  error.
- **Threshold counting takes at most one signature per key** —
  Package_Management.md §7 step 4, and the easiest thing here to get wrong.

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
p:cmd:awk

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
  refused. It is what binds a signed index to one repository.
- `G` and `E` are checked at Package_Management.md §7's steps 5 and 6, after the
  signatures and never before.
- **`X` and `N` are checked when the header is read**, which is between those
  steps and step 4: the version and the expiry are fields of a header that has
  to be parsed first, and a header from the wrong repository or an unknown
  grammar has nothing worth comparing in it.

### 3.2 A package stanza

| Letter | Value | |
| --- | --- | --- |
| `C` | digest of the package zip | required |
| `P` | name | required |
| `V` | version (§7) | required |
| `S` | the zip's exact size in bytes | required |
| `I` | unpacked size, for a human | optional |
| `T` | description | optional |
| `D` | depends — a dependency list (§6) | optional |
| `p` | provides — a dependency list | optional |
| `i` | install-if — a dependency list | optional |
| `o` | origin — the source package's name | optional |
| `t` | build time | optional |
| `k` | provider priority | optional |
| `g` | trigger globs, space-separated | optional |

`C` and `S` are what Package_Management.md §7's steps 8 and 9 check against.

apk's `A` (arch) is dropped — there is one architecture. Its `U`, `L`, `m` and
`c` are undefined here; the two lowercase ones are ignored, and the two
uppercase ones would make a package unusable, which is §1's rule about
informational fields stated as a fact.

### 3.3 Canonical order, and the package's URL

A writer emits `C P V S I T o t k g D p i`, omitting what it has not got, so
that a round trip is byte-identical. **A reader requires no particular order** —
the canonical one is what makes the round trip defined, not what makes a stanza
readable.

The index is at `<N>/index` and a package at `<N>/<name>-<version>.zip`.
**Derived, never carried**: Package_Management.md §4 says a URL proves nothing,
so a field naming one could only be a second place to be wrong.

---

## 4. The anchor

`/share/pkg/anchor`, shipped in `rootfs.zip` and re-pinned from it at every
version change (Package_Management.md §6). Signature block, empty line, one
stanza.

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

- **Missing or unreadable is a stop.** There is no fallback, and no prompt
  (Package_Management.md §6).
- **A higher `X` refuses the whole file**, as it does for an index (§3.1).
- **`E` is checked against the caller's fixed time** (Package_Management.md §7
  step 1). An anchor that has expired is refused, whatever else it says.
- **Every anchor meets its own `H:root` over its own `K:root`** — the one in
  the archive as much as one walked to. It proves nothing on its own, since
  whoever edits the file edits the keys with it; what it buys is one check
  rather than two, and an anchor amended by hand after signing is refused.
- **An `H` comes once per use and a `K` once.** Two thresholds for one use is a
  threshold nobody can read, and a key listed twice is a key counted twice.
- **A `K` of another algorithm is left alone**; an `ed25519` one whose key is
  not 32 bytes makes the anchor unusable. That asymmetry is what the algorithm
  name is for (Package_Management.md §8).
- **The chain walk is `G`.** A client at anchor 1 reaches anchor 3 by checking 2
  against 1 and 3 against 2 (Package_Management.md §10). Withholding 2 stops the
  walk; it does not let 3 through. `G` must increase and need not increase by
  one: the numbering orders the chain and stops a rollback, and the signatures
  are what carry the trust, so a withheld anchor stops the walk by the signature
  that is missing rather than by the number that is.

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
| `.trigger` | a `/bin/sh` script, for the globs `.PKGINFO`'s `g:` names |

Scripts run as Package_Management.md §11 describes, with apk's argv convention:
the new version, and on an upgrade the old one after it.

- **An unknown top-level dot-entry makes the package uninstallable** — §1's
  uppercase rule applied to an entry name.
- **`.PKGINFO` authorises nothing**; the index does. It exists so `/pkg/db` can
  be written without keeping the index, and one that disagrees with the index
  stanza that vouched for the package is a refusal. It is **required**, and it
  carries §3.2's letters **less `C` and `S`**, which name the archive and
  cannot be inside it — so it is not a whole §3.2 stanza and a reader takes it
  field by field. **`P` and `V` are what must agree**: they choose the store
  directory and the generation's line, and a list field differing by a space
  would refuse a package that is not wrong.

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
- **Re-read the local header to find where the data begins.** Taking the central
  directory's offset is the classic way to get this wrong.
- Methods **0 (store) and 8 (deflate)** only.
- **Step past the CRC-32** (Package_Management.md §7). The digest from the
  signed index is the check, taken over the whole zip before an entry is read.

**The order above is normative**, not a list. The `/` test runs before the name
test, so `../` is *skipped* and not refused; the method is judged after the
local header has been found. A reader that reorders them refuses archives the
other accepts, which is the disagreement this section exists to prevent.

Two rules a reader given a stream needs and one given a buffer does not:

- **Stop at the entry's declared uncompressed size**, and refuse a stream that
  ends before it or runs past it. The declared size is inside the digested
  archive and is therefore as trusted as the archive; the inflated bytes are
  not, because a megabyte of deflate is a gigabyte of output.
- **An entry compressed larger than `SYS_STAGE_MAX`** cannot be read at all:
  `Sys::Inflate` stages its input (System_Calls.md §8). Nothing `tools/pack.py`
  writes comes near it — `rootfs.zip`'s largest entry is 75 KB compressed.

`parseZip` checks neither, and does not need to: `DecompressionStream` hands
back a buffer, so a size check there would run after the bomb had been
materialised, and the one archive it reads is the release's own.

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
name whose providers ship an `awk`. apk's `so:` is dropped.

**An unparseable version marks the dependency broken, not the file** — the
stanza becomes an uninstallable package and every other stanza still reads.

**A token with no name, or an operator with nothing after it, is malformed** —
`=1.2`, `foo>=`, a bare `!`. That is not a broken dependency but a field that is
not a dependency list, and it is the reader's to refuse the record over. A
broken dependency names something and is simply satisfied by nothing.

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

Under `/pkg`, which the archive does not carry (Concept.md §5.1).

```
/pkg/repositories                one URL per line; today, one line
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
time it is used** and never believed for being on disk, which is what lets it
skip a download without skipping a check.

**A generation is a directory**, holding the text and the links together, so one
`Sys::Rename` of `/pkg/active` commits both.

### 8.1 The installed database

`/pkg/db/<name>-<version>`: §3.2's stanza as the index gave it, plus

| Letter | Value | |
| --- | --- | --- |
| `G` | the index version that vouched for this package | required |
| `F` | a directory, relative to the store directory, repeats | |
| `R` | a filename under the last `F`, repeats | |
| `Z` | the digest of the file the last `R` named | |

A writer emits §3.3's order, then `G`, then each `F` followed by its `R` and `Z`
pairs, so the round trip is defined here too.

`G` is what makes a reinstall re-check rather than believe the disk
(Package_Management.md §7). apk's `M:` and `a:` are dropped: they carry uid, gid
and mode, and there are none here.

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
distinct from what was pulled in to satisfy it. `/pkg/repositories` is one URL
per line — and **`pkg` refuses a second line rather than ignoring it**, since
`/pkg/index` is one file and Package_Management.md §7 step 5's floor is one
number, so a second repository would be checked against the first's `G`. A
trailing slash is stripped, `<N>/index` being `//index` otherwise.

In all three a blank line is skipped and a last line without a newline is still
a line; **a file that is not there reads as an empty one**, so a `/pkg` that has
never been written to needs no seeding. Nothing else is a comment: a `#` line
would be a URL nobody could name.

### 8.3 Committing a generation

Building generation `N` is, in order: remove `/pkg/gen/<N>` and make it again,
write its `packages`, make its `bin/` and fill it, write `/pkg/active.new` as a
symlink to `/pkg/gen/<N>`, and **rename that over `/pkg/active`**. Only the last
step is visible to anything else, which is what makes it the commit; a tab that
dies before it leaves a generation directory nothing names.

Every link is written as an absolute path — `/pkg/bin` to `/pkg/active/bin`,
`/pkg/active` to `/pkg/gen/<N>`, and each farm entry to
`/pkg/store/<name>-<version>/bin/<cmd>`. `/pkg` is a fixed top-level name
(Concept.md §5.1) and there is nowhere else for the tree to be. A *reader* of
`/pkg/active` takes the target as written and accepts either spelling, since a
link put there by hand is a link.

Two packages shipping one command leaves whichever the farm wrote last. Making
that impossible is the solver's, not this layer's.

---

## 9. What is deliberately not apk's

| Here | apk | Why |
| --- | --- | --- |
| `Q2` digests only | `Q1`, `X1`, `X2`, a promoted `Q1` | one algorithm, no negotiation |
| the index is text | `APKINDEX.tar.gz` | a tar reader for one member |
| a header stanza with `G` and `E` | neither; a client-side mtime | the policy requires both |
| end of file commits a stanza | a last stanza with no blank line is dropped | silent loss where a file is hand-edited |
| an empty line ends a stanza | any line under two bytes does | a one-character line is malformed |
| a letter means one thing | letters are reused between files | one reader, five files |
| no `><` | a checksum comparison operator | the index names a package by hash |
| no `@tag` | repository pinning | there is one repository |
| no `so:` | an ELF shared-library namespace | every binary here is static |
| no `M:`, no `a:` | uid, gid, mode, xattr digest | there is no permission model |
| a package is a zip | gzip streams forming a tar | `parseZip` exists; a tar reader does not |
| metadata is a dot-entry | an ordered prefix of the tar | a zip's directory has no order to rely on |
| an unknown dot-entry refuses it | unknown control files ignored | unreadable instructions must not half-install |
| `A`, `U`, `L` dropped | uppercase, and required | one architecture; and §1's lowercase rule |

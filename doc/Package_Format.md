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
| `p` | provides — a dependency list, and §6.1's generated names | optional |
| `i` | install-if — a dependency list | optional |
| `o` | origin — the source package's name | optional |
| `t` | build time | optional |
| `k` | provider priority | optional |
| `g` | trigger globs, space-separated (§5.1.1) | optional |

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
| `.trigger` | a `/bin/sh` script, for the globs `g:` names (§5.1.1) |

Scripts run as Package_Management.md §11 describes, with apk's argv convention:
the new version, and on an upgrade the old one after it. A removal passes the
version leaving and nothing after it. Each is spawned as `/bin/sh <file>`, so
the file itself need carry no `#!`.

**A dot-entry other than `.PKGINFO` is kept**, written into
`/pkg/store/<name>-<version>/` under its own name and recorded in §8.1's file
list like any payload file. It has to be: `pre-deinstall` runs at a removal,
when the archive is long gone. Keeping it there rather than beside the record
means `pkg verify` re-hashes it, `pkg files` lists it, and `pkg clean` collects
it with the package, none of which needed a rule of its own. `.PKGINFO` is the
exception because the record supersedes it.

**The commit is the line between `pre-` and `post-`.** Every `pre-` script runs
after each package is fetched, checked and unpacked and before §8.3's rename;
every `post-` script runs after it. So a `post-` script can run what was just
installed and a `pre-` script cannot. apk draws the line at extraction instead;
here nothing is extracted *into place* — the rename is the only moment anything
else can see — so that boundary has no analogue and would mark a moment at which
nothing happens.

A script that fails is recorded (§8.1's `b`) and the transaction carries on.
Package_Management.md §11 says why, and a trigger is a script like the six.

### 5.1.1 `.trigger`, and what wakes it

`.trigger` takes **directories** as argv rather than versions, and runs **once
per package, after the whole transaction** — after the `post-` scripts, which
are themselves after the commit. apk's rule, ported whole.

A transaction has a view of two sets of directories. The **modified** set is
what it wrote: each unpacked package's store directory and each of its `F`s,
plus **`/pkg/bin`**. `/pkg/bin` is there because a removal writes nothing to the
store — the bytes stay for a rollback — but §8.3's farm is rebuilt and
`/pkg/bin` resolves to new contents; it is the equivalent of apk marking a
directory modified when it deletes files from it, and without it a removal could
never wake anything. The rest is every directory of every package the
transaction leaves installed.

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

Two consequences, both easy to read backwards. A `+` glob matching an unmodified
directory **still wakes the trigger** — it only withholds that directory, so a
fresh package whose globs are all `+` runs with an empty argv. And a package
whose globs match nothing does not run its trigger at all.

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
name whose providers ship an `awk` (§6.1). apk's `so:` is dropped.

**An unparseable version marks the dependency broken, not the file** — the
stanza becomes an uninstallable package and every other stanza still reads.

**A token with no name, or an operator with nothing after it, is malformed** —
`=1.2`, `foo>=`, a bare `!`. That is not a broken dependency but a field that is
not a dependency list, and it is the reader's to refuse the record over. A
broken dependency names something and is simply satisfied by nothing.

### 6.1 `cmd:` names

The one generated namespace. A package provides **`cmd:<command>=<V>` for every
entry of its `bin/`**, `<V>` being the package's own version: `hello-1.0-r0`
shipping `bin/hi` provides `cmd:hi=1.0-r0`.

**`bin/`, and flat.** That is exactly the set §8.3's link farm carries — one
link per entry, directories skipped — so `cmd:x` holds precisely when `x` on
`PATH` runs this package's file. `bin/sub/tool` yields nothing, because the farm
never reaches it. A rule naming more would promise a command nobody could type;
one naming less would leave a typeable command unnamed.

**Whether the entry is a program (Concept.md §4) is not asked.** The farm does
not ask either — it links what is there — so a `bin/` entry that is not one is
already on `PATH` and already answers 126. Asking here would make the two sets
differ, and the point of the rule is that they do not.

**The version is what makes the name selectable**, not decoration. A name whose
providers are all unversioned can be depended on but never installed: there is
nothing for a solver to choose between, and the name is virtual. With one,
`pkg install cmd:awk` picks a provider and `cmd:awk>=1.2` compares the providing
package's version.

**The publisher generates them, into the index stanza.** A package need not
declare them and cannot get them wrong; `.PKGINFO` need carry none, since §5.1
requires only `P` and `V` to agree. A `p:` line written by hand is an ordinary
provide and merges with them. `/pkg/db` is written from the index stanza (§8.1),
so an installed package provides these names too, and a solve against the
installed set sees what a solve against the index saw.

**Two packages shipping one command both provide one name, and that is not a
conflict.** Both may be installed, and §8.3's "whichever the farm wrote last"
still decides which runs. What the name adds is that a *dependency* on `cmd:x`
is satisfied by either, with `k` (§3.2) choosing. Making co-installation
impossible is a package's own `!` conflict to declare.

**Nothing special-cases the prefix.** §6's reader, the index lookup and the
solver see a name that happens to contain a colon; there is no clause for
`cmd:` in any of them, and adding one would be the regression.

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

Under `/pkg`, which the archive does not carry (Concept.md §5.1) — bar the
one line of configuration, which sits with the anchor in `/etc` and is
re-pinned by a release like it (Package_Management.md §6).

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
time it is used** and never believed for being on disk, which is what lets it
skip a download without skipping a check.

**A generation is a directory**, holding the text and the links together, so one
`Sys::Rename` of `/pkg/active` commits both.

### 8.1 The installed database

`/pkg/db/<name>-<version>`: §3.2's stanza as the index gave it, plus

| Letter | Value | |
| --- | --- | --- |
| `G` | the index version that vouched for this package | required |
| `b` | the install script that failed (§5.1), without its dot | optional |
| `F` | a directory, relative to the store directory, repeats | |
| `R` | a filename under the last `F`, repeats | |
| `Z` | the digest of the file the last `R` named | |

A writer emits §3.3's order, then `G`, then `b`, then each `F` followed by its
`R` and `Z` pairs, so the round trip is defined here too.

The file list covers §5.1's kept dot-entries as well as the payload — they are
written into the store directory and hashed like anything else — so a script at
the top of the package is an `F` of `""` and an `R` of `.post-install`.

`b` is **lowercase on purpose**. §1: a reader that does not know it ignores it
and loses a warning; one that refused the record over it would lose `pkg list`,
`pkg files` and the installed set the solver is handed. Losing the warning is
the safer failure.

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
distinct from what was pulled in to satisfy it. `/etc/repositories` is one URL
per line — and **`pkg` refuses a second line rather than ignoring it**, since
`/pkg/index` is one file and Package_Management.md §7 step 5's floor is one
number, so a second repository would be checked against the first's `G`. A
trailing slash is stripped, `<N>/index` being `//index` otherwise.

In all three a blank line is skipped and a last line without a newline is still
a line; **a file that is not there reads as an empty one**, so a `/pkg` that has
never been written to needs no seeding, and emptying `/etc/repositories` is how
a system is pointed at nothing. Nothing else is a comment: a `#` line would be a
URL nobody could name.

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

---

## 10. Building a package repository

A tutorial, and the only part of this document addressed to you. Everything
above is what `pkg` reads; this is how to write it.

A repository is **a directory of static files behind any web server**. There is
no server-side code, no database and no upload API: you build the files on your
own machine, sign two of them, and copy the directory up. Given
`N = https://packages.example/braam`, the server holds

```
braam/index                     the signed index
braam/hello-1.0-r0.zip          one file per package
braam/libz-1.0-r0.zip
```

and nothing else. §3.3 derives both URLs from `N`, so a package moves by being
renamed and by nothing else.

The six tools are in `tools/`. All are Python 3; the ones that sign need
`pip3 install cryptography` and nothing else does.

### Step 1 — make four keys

```
python3 tools/ed25519.py root1.key root2.key root3.key index.key
```

Each line printed is a path, the public key and the key's `Q2…` id. **Three
root keys and one index key**, because they do different jobs
(Package_Management.md §5):

- a **root key** signs anchors and nothing else. Make it on a machine that has
  never served the repository, encrypt it with a passphrase kept somewhere else,
  back it up, and copy its key id onto paper. Three of them held by three
  people, two needed, so no single machine can move trust.
- the **index key** signs the index and nothing else. It lives on the machine
  that publishes, unattended, on purpose — and it is cheap to revoke, because
  the root keys were kept expensive.

`ed25519.py` refuses to write over a key that exists. Nothing else in the tree
reads a key, and `pkg` itself never signs.

### Step 2 — sign an anchor

The anchor (§4) names those public keys and the thresholds over them. It is the
one file that is **not** downloaded: it ships inside `rootfs.zip`, so a client
trusts your repository by running your build.

```
python3 tools/mkanchor.py --out anchor --version 1 --expiry 1861920000000 \
    --threshold root=2 --threshold index=1 \
    --key root=root1.key --key root=root2.key --key root=root3.key \
    --key index=index.key \
    --sign root1.key --sign root2.key
```

`--key` names private halves and writes down their public ones; `--sign` names
the private halves that sign. Two signatures because `--threshold root=2` says
so — an anchor must meet its own root threshold. `--expiry` is milliseconds
since the epoch:

```
python3 -c 'import datetime as d; print(int(d.datetime(2029,1,1,
    tzinfo=d.timezone.utc).timestamp()) * 1000)'
```

Copy the result to `rootfs/etc/anchor`, put your repository's URL in
`rootfs/etc/repositories` beside it, and rebuild. Then put `root1.key`,
`root2.key` and `root3.key` back where they came from; publishing does not need
them again until you rotate a key or the anchor's expiry comes round, and each
new anchor carries a higher `--version` than the last.

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
letter of §3.2, `D` (depends) and `T` (description) being the two worth setting.
Versions are apk's grammar (§7), so `-r0` is the release number and `1.0-r1`
supersedes `1.0-r0`.

Three conventions do work for you:

- **`bin/` is what lands on `PATH`.** Every flat entry becomes a link in the
  installed generation's `bin/` (§8.3) and a `cmd:<name>` provide (§6.1). You
  write neither down.
- **A dot-entry is metadata.** `.pre-install`, `.post-install` and the four
  others are `/bin/sh` scripts run around the commit; `.trigger` runs when a
  directory your `g:` globs name changes (§5.1).
- **The zip is reproducible.** Same inputs, same bytes, so a rebuild that
  changes the digest changed something.

### Step 4 — sign an index

One index over every package the repository offers, in one command:

```
python3 tools/mkindex.py --out index --url https://packages.example/braam \
    --version 41 --expiry 1790000000000 \
    --description 'Example packages' --sign index.key \
    hello-1.0-r0.zip libz-1.0-r0.zip
```

It reads each zip: `C` and `S` from the bytes, the rest from `.PKGINFO`, and
§6.1's `cmd:` names from `bin/`. **`--version` must increase** at every
publication — a client refuses an index older than the one it holds (§3.1) — and
`--expiry` is a promise to re-sign before that moment. Pick a period you can
actually keep; a month is normal. An index that has expired stops working, which
is the freeze protection doing its job rather than a fault.

Every package listed must be in the same directory as `index` on the server, and
a package the index does not list cannot be installed however it got there.

### Step 5 — copy it up, and try it

Upload `index` and the zips together — nothing serves an index whose packages
are not beside it. On the client:

```
echo https://packages.example/braam > /etc/repositories
pkg update
pkg install hello
```

`pkg update` checks, in this order (Package_Management.md §7): the anchor's
expiry, the index's signature against the anchor's index keys, the index's
expiry, and its `G` against the one already held. `pkg install` then checks each
package's size and digest against the index stanza that vouched for it. So a
refusal names the step it stopped at, and the common mistakes map to it:

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
  is the routine, and it needs only the index key.
- **Rotating the index key**: make a new one, sign a new anchor naming it with a
  higher `--version`, and ship that anchor in a release. The old index key stops
  being trusted the moment clients take the new anchor.
- **A stolen root key**: below the threshold, sign a new anchor without it. At
  or above it, the anchor has to be replaced out of band — which is a release,
  and the key ids on paper are what let anyone check the new one.

`tools/mkrepo.py` does all five steps in forty lines to build the test fixture,
under keys it destroys afterwards. It is the shortest complete example there is.

### What never leaves your machine

Package_Management.md §9, restated because it is the mistake this whole
document exists to prevent. **No private key** goes into the git tree, into
anything built from it, or inside `rootfs.zip`. The signing tools read a key
from a path, keep nothing, and write nothing but the signature. If a key is ever
in a place a browser could fetch it, it is not a key any more.

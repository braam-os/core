# Braam — Package management

The security specification for `/bin/pkg`: what is trusted, what is checked,
in which order, and what the publisher must do with the keys.

[Package_Formats.md](Package_Formats.md) is the grammar of the files named here.
[Concept.md](Concept.md) is the system specification; where the two disagree,
Concept.md wins. [Release_Notes.md](Release_Notes.md) holds the reasoning.

A bare `§N` is a section of **this** document; a reference elsewhere is written
out, as `Concept.md §5.1`. The design is a cut-down TUF, closest to PEP 458.

Where each rule lives:

| Rule | Code |
| --- | --- |
| §6, §7 steps 2 and 4 | [src/cmd/pkg/trust.cpp](../src/cmd/pkg/trust.cpp) |
| §7 steps 1–9 | [src/cmd/pkg/index.cpp](../src/cmd/pkg/index.cpp) |
| §7 step 10, §11's scripts | [src/cmd/pkg/install.cpp](../src/cmd/pkg/install.cpp) |
| §7.1's sideload | [src/cmd/pkg/local.cpp](../src/cmd/pkg/local.cpp) |
| §8's two syscalls | [src/kernel/sysabi.h](../src/kernel/sysabi.h), `Sys::Verify`, `Sys::Clock` |
| §9's tools | `tools/ed25519.py`, `mkanchor.py`, `mkindex.py` |

---

## 1. Model

A package is a zip file and is **not signed**. It is listed in an *index*, and
the index **is** signed. The index gives each package's digest and exact size.
`pkg` fetches the index, checks its signatures, then fetches a package and
checks its digest. A mismatch discards the package.

That is how a package **from a repository** is named. There is a second way,
and only one: **a person types the archive's path or URL**. §7.1 is what that
buys and what it costs, and the rest of this document is about the first.

---

## 2. Terms

| Word | Meaning |
| --- | --- |
| digest | SHA-256 of a file. |
| private key | The secret half. Signs. Never leaves the owner. |
| public key | The published half. Checks a signature. |
| signature | Proof that a private key's holder saw these exact bytes. |
| offline key | A private key on a machine with no network. |
| online key | A private key on a running server. |
| threshold | How many distinct keys must sign before the result is believed. |
| index | One signed file listing every package, with digest and size. |
| anchor | The file naming the trusted public keys. Where checking stops. |

---

## 3. Threat model

The attacker is **the repository and the network in front of it**: mirrors,
CDNs, and anyone who can answer a request. Assume the attacker can answer every
request, answer differently to different clients, replay old answers, withhold
new ones, and hold the repository's online key.

The attacker is **not** the person at the keyboard. Whoever types a command has
the whole store already (§11); nothing here is a defence against them, and §7.1
is the case where they say so out loud.

| Attack | Stopped by |
| --- | --- |
| Arbitrary install — bytes of the attacker's choosing | §7: the digest comes from a signed index |
| Wrong package — a real package, not the one asked for | §7: the index ties name and version to a digest |
| Rollback — a genuine but older index | §7 step 5: the version only goes up |
| Freeze — a valid index replayed for ever | §7 step 6: the index expires |
| Mix and match — packages that never went together | §7: the index is signed as one file |
| Extra dependency | §7: dependencies are inside the signed index |
| Endless data | §7 steps 3 and 8: every fetch is capped before it starts |
| One stolen online key | §5, §10: revoked by an offline key |

**Denial of service is out of scope.** Whoever can answer a request can refuse
one. What is guaranteed is that a client which cannot update reports it and
never continues quietly.

---

## 4. Trusted base

Trusted without proof:

- the origin serving `index.html`, `kernel.wasm` and `rootfs.zip`, and its TLS;
- the browser — its WebAssembly, OPFS and `fetch`;
- the host JavaScript in `web/`, which starts the kernel, unpacks the archive
  and steps every process (Concept.md §4.3).

A package repository is in none of these.

Signature and digest checking run in the host, through `Sys::Verify` and
`SvcOp` on the existing `host_svc` import (Concept.md §2.2). No new import.

Two things the browser supplies that are **not** trusted:

- **The clock.** `Sys::Clock` reports what the machine believes. §7 step 1
  bounds the damage; §11 states the rest.
- **Redirects.** The browser follows them and does not report them, so **the
  URL a package came from proves nothing**. A package is named by its digest.

---

## 5. Roles and keys

| Role | Kept | Threshold | Signs | Expiry |
| --- | --- | --- | --- | --- |
| **root** | offline | *t* of *n*, *t* > 1 | the anchor | ~a year |
| **index** | online | 1 | the index | days to a month |

The root role signs anchors and nothing else; the index role signs the index and
nothing else. Neither key ever signs a package.

TUF's *snapshot* and *timestamp* roles are absent: there is one index, published
whole by one writer, carrying its own version and expiry. Per-author keys are
absent and arrive, if ever, as a delegation from the index role — no new anchor
is needed for them.

---

## 6. The anchor

Checking stops at the **anchor**: the file naming the root public keys, the
thresholds, and the index keys the root role currently vouches for. Its grammar
is Package_Formats.md §4.

- The anchor is **`/etc/anchor`**, shipped inside `rootfs.zip`, served from the
  same origin as `kernel.wasm`.
- The boot unpack **deletes each top-level directory the archive carries before
  rewriting it**, so `/etc` is replaced wholesale at every version change.
  Therefore the anchor is **re-pinned at every release and cannot be poisoned
  in the store for good** (Concept.md §5.2).
- **`/etc/repositories`** sits beside it: one repository URL per line, and
  `pkg` refuses a file with more than one line. It is configuration a release
  ships and a release puts back.

A public key becomes trusted in exactly two ways: it ships in the archive, or a
person types its full fingerprint into a place the archive does not overwrite.
**There is no trust-on-first-use and no prompt.**

---

## 7. Verification

### What the index must carry

| Field | Without it |
| --- | --- |
| a **version** (`G`) that only increases | an older genuine index can be replayed |
| an **expiry** (`E`) | a valid index can be replayed for ever |
| the repository **URL** (`N`) | a signed index is not bound to one repository |
| a **digest** (`C`) of every package | the repository picks the bytes |
| an **exact size** (`S`) of every package | a fetch has no limit |
| the **whole package set**, signed as one file | unrelated packages can be combined |
| every **dependency** (`D`) | an unwanted package can be slipped in |
| the **algorithm name** of every key and digest | see §8 |

A package file is never signed by itself; withdrawing one costs a re-signed
index and no key operation. The set is signed, not each entry.

### The rule

> **Nothing is unzipped, written to the store, or run before its bytes have
> been named** — by a digest from a signed index, or by a person who typed the
> archive's own path or URL (§7.1).

For a package named the first way, which is every package a repository offers,
that is the whole of the rule: **a repository never chooses the bytes.**

A zip is not self-checking here: both readers step past the CRC-32
(Package_Formats.md §5.2).

### The order

1. **Fix the time once** from `Sys::Clock`, and use that one value for every
   expiry comparison in the run.
2. **Load `/etc/anchor`.** Missing is `Err(NotFound)`, unreadable or of an
   unknown grammar `Err(Invalid)`, expired or short of its own `H:root`
   threshold over its own `K:root` keys `Err(Perm)`. All three stop.
3. **Fetch `<N>/index`**, capped at `INDEX_MAX` (512 KiB). Longer is a failure,
   not a truncation.
4. **Check the index signatures** against the anchor's `index` keys, to the
   anchor's `index` threshold, counting **at most one signature per key**. A
   key's name is recomputed, never trusted as a label. Short of the threshold is
   `Err(Perm)`.
   Then read the header: an `X` other than `1` is `Err(Unsupported)`, and an `N`
   that is not the URL fetched from is `Err(Perm)`.
5. **Check `G`** against the `G` of the stored `/pkg/index`. Lower is a rollback
   and `Err(Perm)`. Equal means nothing to do — not an error, and the stored
   index is left as it is. No stored index is no floor; a stored index that does
   not parse is a refusal, since treating it as zero would erase the check.
6. **Check `E`** against the time from step 1. Expired is `Err(Perm)`.
7. **Now read the package stanzas**, and resolve the name asked for and its
   dependencies entirely inside the file just checked. A name the index does not
   list does not exist and is not looked for elsewhere.
8. **Fetch `<N>/<name>-<version>.zip`**, capped at the exact `S` the index gave,
   and at `PACKAGE_MAX` (4 MiB) whatever `S` says.
9. **Hash what arrived** and compare size and digest against the stanza. A
   mismatch is `Err(Perm)`.
10. **Only now** unzip and install.

Steps 1–7 are `index_check()` and write nothing; steps 8–9 are `index_fetch()`.
The rule's boundary sits between steps 9 and 10 and is the only crossing.

A cached archive in `/pkg/cache` is **re-hashed against the index every time it
is used**, so it skips a download and not a check.

**Any failure abandons the whole operation and names the step it stopped at** —
`clock`, `anchor`, `fetch`, `signature`, `header`, `version`, `expiry`, `read`,
`package`, `digest`. Nothing is half-installed. There is **no `--force`,
`--insecure` or `--no-verify`** in any form, and no way to make a package the
index named skip a step of this list. `pkg -v` traces each request and reply; a
cross-origin refusal (`Err(Perm)`) and a dead network (`Err(Io)`) are reported
apart.

`/pkg/db/<name>-<version>` records the index version `G` that vouched for each
installed package (Package_Formats.md §8.1), so a reinstall re-checks rather than
believing the disk.

---

## 7.1 An archive named outright

`pkg install` takes an operand that is **a path ending `.zip`, or anything
holding `://`**, and installs the archive itself. Anything else is a package
name and §7 above is unchanged for it.

**What names those bytes is the person who typed them.** No index vouched, so
none of §3's attacks is defended: there is no rollback check, no expiry, no
signature, and no claim that this archive belongs with any other. That is not a
weakening of §7 — it is the operator installing software, which §11 says they
may do anyway and by other means.

Two rules keep the first way intact:

1. **The index stays authoritative wherever it speaks.** An archive whose `P`
   and `V` name a version the index lists, at a digest the index did not give,
   is **`Err(Perm)` at step `index`**. A sideload cannot stand in for a
   repository's package.
2. **An archive the index does list is the repository's package.** The digest is
   what is asked, not how the archive arrived — so a package carried in on a
   file or fetched from a mirror is checked exactly as §7 checks one, keeps its
   `G`, and skips only the download. This is the offline case, and it gives up
   nothing.

### What the archive must still be

A sideload is not "any zip". It must be a package (Package_Formats.md §5.1):

- **`.PKGINFO` is required**, must read as §3.2's grammar less `C` and `S`, and
  **carrying either of those two is a refusal** — they name the archive, and
  the archive is in `pkg`'s hands.
- **`P` and `V` must be path components** — no `/`, no `\`, no `..`, no control
  byte. `/pkg/store/<P>-<V>/` and `/pkg/db/<P>-<V>` are built out of them, so
  without this a `.PKGINFO` could write anywhere in the store. The check is in
  `package_read`, so the index's own stanzas meet it too.
- **Every top-level dot-entry must be one §5.1 knows.**

`C` is then the SHA-256 of the archive and `S` its length, taken here rather
than believed, and §6.1's `cmd:` names are **derived from the archive's flat
`bin/`** — what `mkindex.py` would have written into an index stanza, so a
solve against the installed set sees what a solve against an index would have.

### What it is recorded as

**`G` is `0`** (Package_Formats.md §8.1): no index vouched. It is `0` for these
bytes and not for this operand, so rule 2 above records a real `G` when the
index turns out to list them.

- `pkg install` prints `<stem>: unverified: no index vouches for it` **before it
  acts**, once per package nothing vouched for.
- `pkg verify` reports `unvouched` and **does not fail over it**: that is how
  the package arrived, not something that went wrong.
- `pkg info` falls back to the `/pkg/db` record for a name no index lists, and
  its `vouched` row reads `no`.

Everything else is a package like any other: it joins `/pkg/world` under the
name its `.PKGINFO` gave — not the path, which may be gone by the next solve —
takes part in dependency resolution, is removed by `pkg remove`, and its
scripts run (§11).

---

## 8. Algorithms

**One signature algorithm, and no negotiation.** The anchor says what is
acceptable; an index offering anything else is refused. Every key and digest
still carries an algorithm name, so a second can be added without a new grammar.

| For | Algorithm |
| --- | --- |
| signatures | **Ed25519** (`Sys::Verify`) |
| digests | **SHA-256**, written `Q2…` |
| the spare slot | ECDSA P-256 — unused; the reason the name field exists |

**A key is named by the SHA-256 of `<algorithm> <base64 key>`**, not by a
filename, and the name is recomputed at every use.

WebCrypto's Ed25519 is present in current Chrome, Safari and Firefox.

> **A missing algorithm means `pkg` refuses to run. It never means `pkg`
> installs without checking.**

That is Concept.md §5.3's *capability struct, not probing*, applied to
cryptography.

---

## 9. Key custody

Publisher obligations. `pkg` never signs anything, holds no private key, and
has no code to make one.

### Root keys

- **Made offline**, on a machine that never served the repository. Nothing
  persists on it afterwards.
- **Encrypted with a passphrase** kept apart from the key.
- **Backed up before first use**, to media stored apart.
- **The key ids written on paper.** §10's last row depends on them.
- **Sign anchors only.** A setup that lets a root key sign an index is a setup
  where the offline key is online.
- *n* holders, *t* needed, **`t` > 1**. Record who holds what. The stock
  arrangement is three keys, two needed (Package_Formats.md §10).

### The index key

Online on purpose, fenced in: it signs the index and nothing else, its use is
logged somewhere an attacker holding it does not control, and it is cheap to
revoke — one anchor, signed by the root threshold.

### Forbidden

**No private key** may be in the git tree, in anything built from it, inside
`rootfs.zip`, anywhere else a browser can reach, on the machine serving the
repository (root keys), or shared between the two roles.

### Expiry

An index nobody re-signs stops working; that is the freeze protection working.
Pick a period the publisher can keep, and treat re-signing as routine.

---

## 10. Rotation and revocation

- **Revoking the index key is one root operation**: sign a new anchor, with a
  higher `G`, without it and naming its replacement.
- **Rotating root keys** is the same, plus one rule: the new anchor is signed by
  a threshold of the **old** root keys *and* a threshold of the **new** ones. A
  client at anchor 1 walks to anchor 3 by checking 2 against 1 and 3 against 2,
  each anchor also meeting its own threshold and expiry (`trust_step`,
  `trust_walk`). Nothing is ever trusted unsigned.
- **Revoking a package costs no key operation** — re-sign the index without it.

### What each theft buys

| Stolen | What it buys | Limited by |
| --- | --- | --- |
| the **index key** | malicious packages that verify | the anchor's expiry, and revocation by the root keys |
| a **minority** of root keys | nothing | the threshold; rotate normally |
| a **threshold** of root keys | everything: the attacker names its own index keys | nothing cryptographic; the anchor must be replaced out of band |

### Recovering from a stolen index key

1. **Revoke it**: a new anchor, signed by the root threshold, naming a fresh
   index key. Ship it — here that means cutting a release.
2. **Rebuild the index** by comparing against the last version from before the
   theft; treat anything added, changed or removed after that as suspect.
3. **Publish**, `G` advanced, `E` fresh, signed by the new key.
4. **Say so publicly.** A client that suddenly cannot verify will look for a
   reason.

### Recovering from a stolen root threshold

All of the above, plus new root keys — and the new anchor cannot be
authenticated by anything the attacker does not also hold. It arrives out of
band, which here is a release from the origin over TLS, checked against the key
ids on paper (§9).

---

## 11. Limits

Each of these follows from a decision made elsewhere in the system.

**`pkg` has no privileges, and there are none here to have.** OPFS stores no
per-file mode, `writable()` is per-mount, every mount but `/proc` is the one
read-write store, and `/bin` is writable. What this document delivers is
**"a repository never chooses what `pkg` installs"**, not "only checked code
runs" — and never did. `/bin/unzip` opens any archive, `curl` fetches any
bytes, and the operator may overwrite `/bin/pkg` itself. §7.1 is that same
freedom spelled as a command rather than left to be assembled by hand, which is
why it is announced, recorded as `G: 0`, and reported by `pkg verify`.

**An installed file carries no lasting guarantee.** Checking happens once, at
install. `pkg verify` re-reads each recorded file and compares it against the
digest recorded then; it reports a file that changed and does not prove that
none did. Anything may overwrite `/bin/pkg` itself.

**A sideload's `.PKGINFO` is unsigned input to the solver.** It is the stanza
(§7.1), so its `D:`, `p:`, `i:` and `k:` are believed: an archive may declare
itself the provider of any name, `cmd:` names included, and `k:` a priority that
outranks a repository's package for one. §8.3's "whichever the farm wrote last"
then decides which file a command name runs. That is the operator's choice
arriving in full, and the two bounds on it are §7.1's — the index still owns
every name-version it lists, and `P` and `V` cannot leave the store.

**A release erases installed programs from `/bin`, not `/pkg`.** The unpack
replaces each top-level directory the archive carries — `bin` and `etc` — so a
locally trusted key in `/etc` and a URL added to `/etc/repositories` go with it
(that is §6's property). `/pkg` is not carried, so the store, the generations,
the symlinks and `/pkg/db` survive, and `PATH` reaching `/pkg/bin` survives too,
its default being the kernel's (Concept.md §4).

**The clock is the user's.** A clock set far enough back makes an expired index
look current, reopening the freeze attack for that one client. Fixing the time
once per run stops a moving clock producing contradictory decisions; nothing
detects a consistently wrong one.

**A repository must send CORS headers or it is unreachable.** A cross-origin
answer the page cannot read is `Err(Perm)` from `web/svc.js`, distinct from a
dead network's `Err(Io)`; `pkg` prints which happened, and `pkg -v` prints the
request and reply around it.

**No source provenance and no reproducible builds.** A signature says who
published the bytes, not what they were built from.

**Install scripts run, and what authorises execution is whatever named the
bytes.** A package may carry `.pre-install`, `.post-install` and their four
relatives plus `.trigger`, and `pkg` spawns each as `/bin/sh <file>` through
`Sys::Spawn` (Package_Formats.md §5.1). A script may touch **everything the
person who typed `pkg install` may touch** — the whole store.

For a package from a repository that is a signature: a script runs only after
its package's digest matched a digest from a signed index, so the code that runs
is the publisher's and never the network's. **A repository that can rewrite a
package still cannot make one run.**

For a sideload (§7.1) it is the person who typed the path. Refusing to run those
scripts would be theatre — the payload lands in `/pkg/bin` and on `PATH`
either way, so the archive gets to run code the moment anything invokes it. What
the operator is trusted with here is what they are trusted with below.

Two rules follow. A failing script marks its package broken (`b` in
Package_Formats.md §8.1) and the transaction carries on, which is what gives
`pkg verify` something to find. And a script is not how a package installs its
files: `pkg` unpacks those itself, so a package that only places files runs no
code at all.

**Denial of service stays available** to anyone on the path (§3). The guarantee
is that a client which cannot update knows it.

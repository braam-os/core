# Braam — Package management

How software that did not ship with the system is fetched, checked and
installed, and how the signing keys are looked after.

**This was written before `/bin/pkg` existed**, because these decisions cannot
be taken back later: a key made on a networked machine is never afterwards an
offline key. The code now satisfies it, and
[Package_Format.md](Package_Format.md) is the grammar it satisfies it in — but
the order still matters, and this document is where a disagreement is settled.

[Concept.md](Concept.md) is the specification and this does not replace it.
Where the two disagree, Concept.md wins. [Release_Notes.md](Release_Notes.md)
holds the arguments: this file says what the rules are, that one says why.

A bare `§N` below is a section of **this** document. A reference to the
specification is written out in full, as `Concept.md §5.1`.

The design is a cut-down version of The Update Framework (TUF). PEP 458, which
applies TUF to PyPI, is the closer model.

---

## 1. The idea in one paragraph

A package is a zip file, and **it is not signed**. Instead it is listed in an
*index*, and the index **is** signed. The index gives each package's hash and
exact size. `pkg` fetches the index, checks the signature, then fetches a
package and checks its hash. If the hash does not match, the package is thrown
away. Everything below is that paragraph with its reasons attached.

---

## 2. Words used here

| Word | Meaning |
| --- | --- |
| hash | A short fingerprint of a file. Change one byte and the hash changes. Two different files cannot be made to share one. |
| private key | A secret. Used to sign. Never leaves the owner. |
| public key | Derived from a private key. Used to check a signature. Safe to publish. |
| signature | Proof that the holder of a private key saw this exact data. |
| offline key | A private key kept on a machine with no network. Hard to steal, awkward to use. |
| online key | A private key on a running server. Easy to use, easier to steal. |
| threshold | How many separate keys must sign before we believe the result. |
| index | One signed file listing every package, with each one's hash and size. |
| anchor | The file naming the public keys we trust. Where checking stops. |

---

## 3. What we defend against

The attacker is **the place packages come from and the road they travel**: the
repository, its mirrors, any CDN in front of it, and anyone who can answer for
the network. Assume the attacker can answer every request, answer differently to
different people, replay old answers, withhold new ones, and hold the
repository's online key.

The attacker is *not* the browser or our own web site — see §4.

| Attack | What it tries | Stopped by |
| --- | --- | --- |
| Arbitrary install | send bytes of its own choosing | §7 — the hash comes from a signed index |
| Wrong package | send a real package, but not the one asked for | §6 — the index ties name and version to a hash |
| Rollback | send a genuine but older index, to bring back a fixed bug | §6 — the version number only goes up |
| Freeze | keep replaying a valid index, so a fix never arrives | §6 — the index expires |
| Mix and match | combine packages that never went together | §6 — the index is signed as one file |
| Extra dependency | slip in a package nobody asked for | §6 — dependencies are inside the signed index |
| Endless data | answer with a stream that never ends | §7 — every fetch is capped before it starts |
| One stolen key | sign with a key it took | §5, §10 — the online key can be revoked by an offline one |

**Denial of service is not on the list.** Whoever can answer a request can also
refuse one, and signing cannot change that. What we do get is that a client
which cannot update **says so**. It never continues quietly.

---

## 4. What is already trusted

Worth writing down what we believe without proof:

- **our own origin**, which serves `index.html`, `kernel.wasm` and
  `rootfs.zip`, and the TLS to it;
- **the browser** — its WebAssembly, its OPFS, its `fetch`;
- **the host JavaScript in `web/`**, which starts the kernel, unpacks the
  archive and steps every process (Concept.md §4.3).

A package repository is in none of those. That is the point of signing it.

### Why the checking may run in JavaScript

Signature and hash checking use **WebCrypto** (`crypto.subtle`), reached through
new operations on the existing `host_svc` import. WebCrypto returns promises, so
it fits Concept.md §2.2 and needs no new import — only an enum value on each
side (`src/svc/svc.h`).

The obvious worry is that this puts the checker inside the host. It costs
nothing, because **the host is already trusted completely**: it hands the kernel
every byte of `kernel.wasm`, of the archive, and of every process image. A host
willing to lie about `crypto.subtle.verify` could simply supply a different
`pkg.wasm` instead. Nothing is widened.

### Two things the browser hands over that we still do not trust

- **The clock is the user's.** `SvcOp::Clock` reports what the machine believes,
  and a machine can believe anything. §7 limits the damage; §11 admits the rest.
- **Redirects are invisible.** The browser follows them and does not report
  them, which is why `curl` has no `-L`. So **the URL a package came from proves
  nothing.** A package is named by its hash and by nothing else.

---

## 5. The two keys

| Role | Kept | Threshold | Signs | Expires |
| --- | --- | --- | --- | --- |
| **root** | offline | *t* of *n*, *t* > 1 | the anchor: which public keys we trust | a year |
| **index** | online | 1 | the index | days |

The root key is used almost never: to name the index key, to replace it, and to
replace itself. The index key is used at every publication.

**Why two keys and not TUF's four.** TUF adds a *snapshot* role and a
*timestamp* role. Both exist to serve a busy repository with many writers.
Snapshot stops a mix of metadata that never went together; timestamp limits
replay without forcing the big files to expire quickly. Here there is one index,
published whole, by one writer, carrying its own version and its own expiry, so
both properties come from the index itself. A second role would only restate it.

What we give up, plainly: there is no arrangement where an attacker must steal
two separately held online keys, and one expiry sets both how often we re-sign
and how long a replay can last. TUF's split is the answer if either becomes a
problem.

**Per-author keys are missing on purpose, and the design leaves room for them.**
That is PEP 480's stronger model, where the author signs and the repository
cannot forge. It arrives as a delegation from the index role: a change to what
the index carries, not a new root key. That is why cutting down is safe. It
grows back without a new anchor.

---

## 6. Where the trusted keys come from

Every signature check has to stop somewhere. It stops at the **anchor**: the
file naming the root public keys, the threshold, and the index keys the root key
currently vouches for.

**The anchor ships inside `rootfs.zip`.** It is a file in `rootfs/` in the git
tree, packed by `tools/pack.py`, served from the same origin as `kernel.wasm`
over the same TLS, and unpacked into the store at boot by the code that installs
`/bin`.

It goes in **`/etc/anchor`**. `/etc` is already a top-level directory the
archive carries, so Concept.md §5.1's mount layering does not change.

That gives us something useful for free. The unpack in `web/fs.js` **deletes
each top-level directory the archive carries before rewriting it**, so `/etc` is
replaced wholesale at every version change. Therefore:

> **The anchor is re-pinned from the archive at every version change, and cannot
> be poisoned in the store for good.**

Concept.md §5.2's rule — *the archive, not the store, is what the system
recovers from* — covers the trust anchor too. This is also what makes §10's
worst case survivable: replacing the anchor out of band means cutting a release,
and we already cut releases.

`/etc/repositories` beside it is the same bargain. The list of URLs an update
reads is configuration a release ships and a release puts back, not state a
package manager accumulates — so it sits with the anchor rather than in
`/pkg`, and pointing the system somewhere else is an edit a version change
undoes.

The cost of the same behaviour is in §11: a key trusted *locally* in `/etc` is
wiped by the same unpack, and so is a URL added there.

### There is no prompt

A public key becomes trusted in exactly two ways:

1. **It ships in the archive.** This is how our own repository is trusted.
2. **A person types its full fingerprint**, and `pkg` records it somewhere the
   archive does not overwrite.

There is **no trust-on-first-use** and no "this repository offers key X,
accept?" A question the user cannot possibly answer has only one answer, yes,
and asking it turns the whole check into a formality.

---

## 7. What the index says, and how `pkg` checks it

The formats are [Package_Format.md](Package_Format.md), written to satisfy this
section. What is settled here is what the index must contain, because each item
stops a named attack.

| The index carries | Without it |
| --- | --- |
| a **version number** that only goes up | an older genuine index can be replayed |
| an **expiry** | a valid index can be replayed for ever |
| a **hash** of every package | the repository picks the bytes |
| an **exact size** of every package | a fetch has no limit |
| the **whole package set**, signed as one file | packages that never went together can be combined |
| every **dependency** | an unwanted package can be slipped in |
| the **algorithm name** of every key and hash | see §8 |

**A package file is never signed by itself.** It is named by hash from the
signed index. apk v2 does the opposite: each `.apk` carries its own signature.
Naming by hash is better here for one reason. Withdrawing a package costs a
re-signed index and **no key operation at all**.

**The set is signed, not each entry.** Otherwise an attacker could serve a real
signature for a new package A next to a real signature for an old package B, a
pair the publisher never produced.

### The rule

> **Nothing is unzipped, written to the store, or run before its hash matches a
> hash from a signed index.**

The zip format does not help. `parseZip` in `web/fs.js` reads each entry's
flags, sizes and name and steps straight past the CRC-32, so **a zip here is not
self-checking**. That is not a gap to close: a CRC is not a security check, and
whoever chooses the bytes also chooses a matching CRC.

### The order

1. **Fix the time once**, from `SvcOp::Clock`, and use that one value for every
   expiry comparison. A clock that moves mid-run must not make two checks
   disagree.
2. **Load the anchor** from `/etc/anchor`. Missing or unreadable: stop. There is
   no fallback and nothing to rebuild it from.
3. **Fetch the index**, up to a cap `pkg` chooses. Longer than the cap is a
   failure, not a truncation.
4. **Check the index signatures** against the index keys the anchor names, to
   the anchor's threshold, counting **at most one signature per key**. Otherwise
   one signature repeated meets any threshold.
5. **Check the version** against the highest seen before. Lower is a rollback
   and a failure. Equal means nothing to do, and is not an error.
6. **Check the expiry** against the time from step 1. Expired is a failure.
7. **Now read the index** — resolve the name asked for and its dependencies
   entirely inside the file just checked. A name the index does not list does
   not exist, and is not looked for elsewhere.
8. **Fetch each package**, capped at the exact size the index gave.
9. **Hash what arrived.** Compare hash and size against the index.
10. **Only now** unzip and install.

The rule's boundary sits between steps 9 and 10, and that is the only crossing.

**Any failure abandons the whole operation and reports why.** Nothing is
half-installed. There is no `--force`, `--insecure` or `--no-verify` in any
form: a flag that skips checking is a flag the attacker's instructions will tell
the user to pass.

**`pkg` keeps its own record** of what it installed and which index version
vouched for it, so a reinstall re-checks rather than believing the disk.

---

## 8. Algorithms

**One signature algorithm, and no negotiation.** An algorithm the repository
gets to choose is an algorithm the repository can weaken. The anchor says what
is acceptable; an index offering anything else is refused.

**Every key and hash still carries an algorithm name.** Not to negotiate, but so
a second algorithm can be added later without rewriting this document.

| For | Algorithm |
| --- | --- |
| signatures | **Ed25519** — small keys, small signatures, no parameters to get wrong |
| hashing | **SHA-256** |
| the spare slot | ECDSA P-256 — unused; the reason the name field exists |

**A key is named by the SHA-256 of its public key**, not by a filename. apk v2
required the public key's *filename* to match the signature and apk v3 dropped
that for an intrinsic key identity. No point repeating the first attempt. A name
that is a hash of the key cannot be claimed by a different key.

**On availability.** WebCrypto's Ed25519 arrived much later in Chrome than in
Safari and Firefox — Chrome 137, in 2025 — so it is present across the three by
the time `pkg` needs it, and `Sys::Verify` is built on that. The refusal below
is still what an older engine gets, and is the point of the paragraph:

> **A missing algorithm means `pkg` refuses to run. It never means `pkg`
> installs without checking.**

This is Concept.md §5.3's *capability struct, not probing* applied to
cryptography. A
package manager that cannot check is not a weaker package manager. It is a
downloader, and we already have `curl`.

---

## 9. Looking after the keys

### The root key

- **Made offline**, on a machine that never served the repository and has no
  network. Nothing persists on it afterwards.
- **Encrypted with a strong passphrase**, and the passphrase kept apart from the
  key.
- **Backed up before first use**, to media stored apart from each other. Destroy
  the medium it was made on if it was not volatile.
- **The anchor's hash written on paper.** Thirty-two bytes, never needed in a
  hurry, and the one thing that survives losing everything else. §10's last row
  depends on it.
- **Signs anchors and nothing else.** It never signs an index. A setup that lets
  it is a setup where the offline key is online.

### Holders and threshold

*n* holders, each with a key of their own, *t* of them needed, and **`t` > 1**,
so no one person and no one compromised machine can move trust. Write down who
holds what and keep it current. A threshold nobody can currently meet is a
threshold of zero on the day it is needed.

### The index key

**Online, on purpose.** Publishing continuously needs a key a machine can use
with nobody present, and pretending otherwise produces a process nobody follows.
The concession is fenced in:

- it signs the index and nothing else;
- its use is logged, somewhere an attacker holding the key does not also
  control;
- it is **cheap to revoke** — one anchor, signed by the root keys. It is cheap
  because the root key was kept expensive.

### Forbidden

Spelled out, because this is the mistake the document exists to prevent. **No
private key** may be:

- in the git tree, or in anything built from it;
- inside `rootfs.zip`, or anywhere else a browser can reach;
- on the machine serving the repository, in the root key's case;
- shared between the two roles. One key doing both jobs is one role.

**`pkg` never signs anything.** It holds no private key and needs no code to
make one. Signing happens on a publisher's machine, not in a browser tab.

### Expiry is a duty, not a nuisance

An index nobody re-signs stops working. That is the freeze protection doing its
job. The alternative is an index valid for ever, which is an attacker's replay
window with no end. So pick a period the publisher can actually keep, and treat
re-signing as routine.

---

## 10. Rotation and compromise

### Rotation

**Revoking the index key is one root operation**: sign a new anchor without it,
naming its replacement.

**Rotating the root keys is the same, with one extra rule.** The new anchor is
signed by a threshold of the **old** root keys *and* a threshold of the **new**
ones, and anchors are numbered. A client last seen at anchor 1 can then walk
forward to anchor 3, checking each against the one before it and against itself.
Nothing is ever trusted unsigned. This is TUF's key-migration rule, and the
double signature is all of it.

**Revoking a package costs no key operation** — re-sign the index without it.
That is §7's argument for not signing packages one by one.

### What each theft buys

| Stolen | What it buys | Limited by |
| --- | --- | --- |
| the **index key** | malicious packages that verify | the anchor's expiry, and the moment the root keys revoke it |
| a **minority** of root keys | nothing | the threshold. Rotate normally, no rush |
| a **threshold** of root keys | everything: the attacker names their own index keys | nothing cryptographic. The anchor must be replaced out of band |

The middle row is why `t` > 1, and why the root keys sit with different people
on different machines.

### Recovering from a stolen index key

1. **Revoke it.** A new anchor, signed by the root threshold, naming a fresh
   index key. Ship it — here that means cutting a release, since the anchor
   lives in the archive.
2. **Work out what the index should say**, by comparing against the last version
   from before the theft. Treat anything added, changed or removed after that as
   suspect until it is rebuilt from sources the attacker did not hold.
3. **Publish a new index**, version advanced, expiry fresh, signed by the new
   key.
4. **Say so publicly.** A client that suddenly cannot verify will look for a
   reason, and no reason looks exactly like an attack in progress.

### Recovering from a stolen root threshold

All of the above, plus new root keys — and the new anchor cannot be
authenticated by anything the attacker does not also hold. It has to arrive out
of band.

**That is survivable here only because of §6.** The anchor ships in
`rootfs.zip`, which ships in a release, served from our origin over TLS. Cutting
a release **is** the out-of-band channel, and it already exists. It is how the
system itself is delivered. The paper copy from §9 is what an outsider checks
that release against.

---

## 11. What this does not protect against

Each of these is a consequence of a decision made elsewhere in the system, not a
bug waiting to be fixed.

**`pkg` has no privileges, and there are none here to have.** OPFS stores no
per-file mode, `writable()` is per-mount, every mount but `/proc` is the one
read-write store, and `/bin` is writable — `rm /bin/sh` already works. So what
this document delivers is **"`pkg` installs only what it checked"**. It is *not*
"only checked code runs". That would need a privilege boundary the system does
not have, and claiming it would be worse than not having it.

**An installed file carries no lasting guarantee.** Checking happens once, at
install. Nothing re-reads the store afterwards, and anything can overwrite
`/bin/pkg` itself. §7's record makes a *reinstall* meaningful. It does not make
the store tamper-evident.

**A version change erases installed packages.** The unpack replaces each
top-level directory the archive carries, and `bin` and `etc` are exactly those.
Anything `pkg` put in `/bin` is gone at the next release, along with a locally
trusted key in `/etc` and a URL added to `/etc/repositories`. For those two this
is the property §6 relies on. It is also why `pkg`'s *record* of what it
installed belongs in a directory the archive does not carry, so a wipe is fixed
by reinstalling, and re-checking.

`/pkg` is that directory. The archive names `bin` and `etc` and no other, so the
store, the generations and the symlinks that activate one all survive an
unpack — and `PATH` reaching them survives too, because its default is the
kernel's (Concept.md §4) and not a file in the store. What a release replaces is
the system; what it leaves standing is what `pkg` installed.

**The clock is the user's, and expiry depends on it.** A clock set far enough
back makes an expired index look current, which reopens the freeze attack for
that one client. Fixing the time once per run (§7) stops a moving clock
producing contradictory decisions; nothing detects a consistently wrong one. The
check is still worth making. It stops replay against every client whose clock is
roughly right, which is nearly all of them.

**A repository must send CORS headers or it is simply unreachable.** A
cross-origin answer the page cannot read comes back as `Err(Perm)` from
`web/svc.js`, which tells a refusal from a dead network. So "serve
`Access-Control-Allow-Origin`" is part of what being a repository here means,
and `pkg` says which of the two happened rather than leaving the publisher to
guess: `Err(Perm)` prints that the server did not grant cross-origin access and
`Err(Io)` prints `no answer`. `pkg -v` prints the request and the reply around
it.

**No source provenance and no reproducible builds.** A signature says who
published the bytes. It says nothing about what they were built from, by whom,
or whether the same source yields them again.

**Install scripts run, and a signature authorises execution.** A package may
carry `pre-install`, `post-install` and their four relatives, and `pkg` runs
each as an ordinary `/bin/sh` process through `Sys::Spawn`. So the honest
statement of what a script may touch is: **everything the person who typed
`pkg install` may touch**, which is the whole store — `/home`, `/bin`, `/pkg`
itself. That is not a concession granted to scripts. It is the first paragraph
of this section restated: there is no privilege boundary here to put one behind,
so a fence drawn around a script would be a drawing and not a fence, and writing
it down would claim exactly the guarantee this section exists to disclaim.

What that costs is the whole of the list above, brought within reach of a signed
package instead of only a mistaken one. What it does not cost is §7's rule,
which is unchanged: a script runs only after its package's hash matched a hash
from a signed index, so the code that runs is the publisher's and never the
network's. **The check moves nothing; it is what the script's authority is
traced back to.** A repository that can rewrite a package still cannot make one
run, and that was the property being bought all along.

Two smaller rules follow. A failing script marks its package broken and is
recorded rather than aborting the transaction — apk's behaviour, and what gives
`pkg verify` something to find. And a script is not how a package installs its
files: `pkg` unpacks those itself, from bytes it hashed, so a package that only
places files runs no code at all and the common case keeps the stronger
property.

**Denial of service stays available** to anyone on the path (§3). The guarantee
is that a client which cannot update knows it, not that it can.

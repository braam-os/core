# Braam — Testing

How the tests are organised and the rules that keep them working.
[Concept.md](Concept.md) is the specification and a bare `§N` below is a section
of it; [Release_Notes.md](Release_Notes.md) holds the arguments.

---

## 1. The three cases

`make run` is `ctest`, and there are three names:

| Name | What runs | What it proves |
|---|---|---|
| `smoke` | `test/run.mjs --kernel kernel.wasm rootfs.zip <bin>...` under Node | the shipping kernel, booted, driven by keystrokes, running real programs |
| `unit` | `test/run.mjs --tests tests.wasm rootfs.zip` under Node | the kernel's own code, called directly, below the level of a program |
| `size` | `tools/size_budget.txt` | `kernel.wasm` is inside its budget |

One at a time: `ctest --test-dir build -R smoke --output-on-failure`.

The whole of `smoke` takes about a second. Node stands in for the browser —
instantiating a freestanding module needs nothing browser-specific — and
[test/fakefs.mjs](../test/fakefs.mjs), [test/fakesvc.mjs](../test/fakesvc.mjs)
and [test/fakeworker.mjs](../test/fakeworker.mjs) stand in for OPFS, the host
services and the process workers. They answer synchronously from inside the
import, and they take their constants, encoders and archive unpacker from
`web/fs.js`, `web/svc.js`, `web/abi.js`: **a format is never restated.**
`rootfs.zip` is passed to both suites so that the two readers of one format —
`src/cmd/pkg/zip.cpp` in wasm and `web/fs.js` in JS — are compared against each
other rather than each trusted against its own idea of it.

## 2. The dividing line

**The in-wasm suite cannot run a program, and the shell is one.** It reaches
everything *below* a program, and anything that needs one to run belongs in the
smoke suite. That is enforced by the build rather than by convention: the pure
sources are **compiled straight into `tests.wasm`** instead of being linked from
their libraries, so a syscall in one of them is a link error.

Compiled in, and therefore testable there: `sh/parse.cpp`, `sh/tokenize.cpp`,
`sh/expand.cpp`, `sh/match.cpp`, `sh/cond.cpp` (the grammar, the expander, the
matcher, `test`'s expression), `proc/opt.cpp` and `proc/time.cpp` (the option
parser and the calendar), and the syscall-free half of `src/cmd/pkg/`.
`pkg/trust.cpp` and `pkg/index.cpp` qualify by taking a `PkgHost`: the suite
hands them the kernel's own services where `/bin/pkg` hands them syscalls.
`pkg/unzip.cpp`, `store.cpp`, `host.cpp` and `install.cpp` stay out.
`sh/glob.cpp` and `sh/condrun.cpp` stay out for the same reason — they walk the
store. Each group carries its reason in
[test/CMakeLists.txt](../test/CMakeLists.txt); keep it there.

## 3. The in-wasm suite

One file per subject in [test/unit/](../test/unit/), and three edits to add one:

1. `test/unit/test_<subject>.cpp`, defining `void test_<subject>()`. It opens
   with `test_begin("<subject>")` and asserts with `CHECK` / `CHECK_EQ` from
   [test/unit/harness.h](../test/unit/harness.h). Helpers go in an anonymous
   namespace above it; `run_now(Task<T>)` takes the value of a task known not to
   suspend, which every in-memory filesystem the suite mounts is.
2. A line in `add_executable(tests …)` in `test/CMakeLists.txt`.
3. A declaration and a call in [test/unit/main.cpp](../test/unit/main.cpp).

**The call order in `main.cpp` is load-bearing**, and every entry that depends
on an earlier one says so beside it — `test_text(); // after screen: it
round-trips through the grid`. Keep that habit.

Fixtures that are tables live beside the cases as `.data` files (`solve.data`,
`version.data`, `index.data`, `repo.data`, `anchor.data`), read as `@name`
blocks. `FakeHost` ([test/unit/fakehost.h](../test/unit/fakehost.h)) is the
`PkgHost` stand-in. There is **no filter**: the case list is inside the wasm, so
running one case means building `tests` and reading the harness output.

## 4. The smoke suite

[test/run.mjs](../test/run.mjs) is the ordered case list and nothing else. Every
case is one file in [test/smoke/](../test/smoke/) exporting a single
`check()`, and takes its kernel, screen and shell from
[test/smoke/harness.mjs](../test/smoke/harness.mjs) — the one owner of the
instance, the grid address, the tracked cwd and the two fakes. The eight `pkg`
cases share [test/smoke/pkgfix.mjs](../test/smoke/pkgfix.mjs) as well: the
fixtures, the two repositories, one clock and the four shapes they assert in.

The harness gives a case `submit(text, now)` to type a line and press Enter,
`run(now)` to tick, `screen()`, `rows`, `row`, `cell`, `words` and `output` to
read the grid back, `prompt(status)` and `chdir()` for the shell's cwd,
`regrid()` for a resize, and `fail(msg)` to stop. `shows(base, step)` is the
clock and the two comparisons most cases want:

```js
const t = shows(13173);
t.is("echo /bin/l*", "/bin/less /bin/ln /bin/ls");  // all of it, joined with |
t.has("echo $x", "one");                            // one row among all of them
```

## 5. The rules the smoke suite runs by

It is **one cumulative session**, not a set of independent tests. That is what
makes it worth having — a shell that has been running for four thousand
keystrokes is the thing under test — and it is what the rules are about.

1. **One boot.** The kernel is instantiated once and driven forward. The seven
   further instantiations are the point of the cases that make them — six in
   `persist`, which is about surviving a reload, and one in `pkg-crash`, which
   is about a tab dying mid-transaction.
2. **`language` is last.** It runs `exit 7`, which ends the shell for good.
3. **`respawn` is indivisible.** Every block in it takes a worker away, and they
   must be **at least a second apart on the `run()` clock** or `RESPAWN_TRIES`
   and `RESPAWN_FLOOR_MS` in `src/user/boot.cpp` see a crash loop.
4. **Timestamps are literal and hand-picked.** Do not fit an automatic clock
   over them: `boot` asserts exact tick deltas, `procfs` asserts a one-second
   `vmstat` interval, and `jobs` waits for a timer to fire. A case picks a base
   in the gap after the one before it and steps by 0.005 or 0.01.
5. **Store contents cross cases.** `/home/notes` is written long before
   `persist` reads it; `cwd` leaves the shell in `/home`; `pkg-install` leaves
   `/pkg` broken on purpose because `pkg-remove` starts from that.
6. **Counters are absolute running totals.** `store.unpacks`, `logged.length`
   and `ticks()` are asserted as exact numbers, so `logged` and `presented` are
   single instances in the harness rather than per-case.
7. **Geometry is restored.** A case that resizes away from 60×16 puts it back.
8. **The archive is optional in form only.** Cases marked `ARCHIVE` in the table
   are skipped without `rootfs.zip`, but a kernel with no archive does not reach
   a prompt, so the suite is run with one.

## 6. Adding a case

- **Below a program** — a new file in `test/unit/`, per §3. Order it by what it
  stands on and say so in the comment.
- **A program, the shell, the screen or a key** — a new file in `test/smoke/`
  and a line in the `CASES` table in `test/run.mjs`, placed where the state it
  needs already exists. Pick a clock base after its neighbour's.
- **A new program or builtin updates `rootfs/share/help` in the same commit.**
  That document is the whole of `help`, nothing notices at run time when it goes
  stale, and the `help` case fails on a forgotten line.
- **A change to the wasm ABI updates `test/smoke/abi.mjs` in the same commit** —
  the six imports and nine exports, and every binary's three imports and four
  exports, are asserted there by name.

## 7. Running part of it

```
node test/run.mjs --list                       # the case names, in order
node test/run.mjs --kernel … --upto=vars       # run through `vars` and stop
```

`--upto` is a prefix, not a filter, and there is no way to run one case alone:
by §5 the state each case leaves is the state the next one needs. It is for
iterating — failing fast at the case being edited, without the output of the
thirty after it. CI passes neither flag. The in-wasm suite has no filter at all
(§3).

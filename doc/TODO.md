# TODO — every program at tier 3

Moving all of `src/cmd/` to the own-worker tier, the shell included. The tier is a claim in a
binary's `braam` custom section and nothing else (Concept.md §4.3: *"the same binary runs at
either tier"*), so no C++ changes: not `sysabi.h`, not `src/proc/`, not `src/sh/`, not
`exec.cpp`, not one file in `src/cmd/`. The work is the host's half, the pool, the tests, and one
design question about the shell.

The order below is deliberate. Measurement comes before commitment, the pool is fixed before
everything starts competing for it, and the shell is last because it is the only step that
amends the specification.

## T1. Measure the two tiers, before anything moves

The reason for today's split is the cost of a syscall, so that number decides how much of the
rest is worth doing.

- Time a scripted run — `wc` over a large file, a three-stage pipeline, and a burst of
  keystrokes at the prompt — with the program at tier 2, then at tier 3.
- Two figures matter: microseconds per syscall round trip, and the added latency of one
  keystroke. A keystroke costs about six syscalls (`key_read`, then a repaint of four), and
  `SYS_CHUNK` is 512, so `cat` of 100 KB is some 400 round trips.
- `make serve` for the real thing; `test/run.mjs` counts ticks but not wall time, and its links
  have no thread in them.

**Done.** `make bench` is the harness: `web/bench.html` drives the shipped page against three boot
archives — `bundle.bin`, and the two tier-3 twins the cmake `bench` target packs by re-stamping —
and `tools/bench.mjs` collects what it posts. Measured at 0.2.44-e6a8552 on an 8-core Mac, ten
timed runs after two warm-ups, medians.

### The two figures

**A syscall round trip costs 34–44 µs at tier 3.** From `wc` over one file against `wc` over
eight, which differ in nothing but how many round trips they make: ΔT over a ΔN of 297 counted
round trips, so the spawn, the compile-cache hit, the instantiate, the exit and the prompt redraw
all subtract out.

| | Blink (Vivaldi) | Gecko (Firefox) | WebKit (Safari) |
|---|---|---|---|
| tier 2, as it runs today | 40.2 µs | 80.8 µs | 5.1 µs |
| tier 2, less the timer wait below | 6.2 µs | 16.8 µs | 1.7 µs |
| tier 3 | 42.3 µs | 33.7 µs | 33.7 µs |

So §4.4's *"order 0.1 ms"* is two to three times pessimistic, and the tier costs about **30–40 µs
per round trip** — `wc` of 86 KB goes from 4.2 ms to 11.4 ms in Blink, 6.5 to 12 in WebKit.

**A keystroke costs 0.2–2.9 ms more with the shell at tier 3.** Paced, one key in flight, from the
key to the last repaint it caused:

| | Blink | Gecko | WebKit |
|---|---|---|---|
| tier-2 shell | 0.17 ms | 1.66 ms | 0.36 ms |
| tier-3 shell | 0.63 ms | 1.89 ms | 3.26 ms |
| 64 keys back to back, per key | 0.10 → 0.34 ms | 0.42 → 2.70 ms | 0.27 → 5.09 ms |

No keystroke was dropped at either tier in any engine. A single key stays inside a frame
everywhere; **sustained typing does not** — 5 ms a key in WebKit and 2.7 in Gecko is a shell that
feels slow while it is being typed into, and that is the number T7 and T8 have to answer to.

The `sh`-at-tier-2 arm is the control: its keystroke figures match the tier-2 arm in all three
engines to within the clock, which is what says the cost is the *shell's* tier and not the
programs'.

### What else the run found, for T2 and T6

- **Tier 2's bulk cost today is mostly a timer, not a call.** Every 64th tier-2 step takes
  `setTimeout(drain, 0)` instead of a microtask, and one of those waits 1.3 ms in Blink, 2.4 ms in
  Gecko and 0.25 ms in WebKit. `wc` over eight files takes that route 8 times: 10.2 ms of a 16.2 ms
  command in Blink, 19 of 33 in Gecko. Tier 3 never takes it. That is why the two tiers look so
  close above, and it is a cheaper thing to fix than T6's ABI change — **T2 should re-decide the
  every-64th rule before anything else.**
- **The pool is one worker short of a pipeline.** `cat | cat | wc` at tier 3 hires one worker and
  terminates one on *every* run, in all three engines, while reusing two. `MAX_IDLE` is 2 and a
  three-stage pipeline plus the shell wants four. This is exactly T2.
- **A tier-3 boot needs the previous kernel to be gone.** WebKit will not finish booting a second
  kernel while the first one's worker still holds its OPFS handles; the harness gives it a fresh
  document and two seconds, and retries. Worth knowing before T8 makes `/bin/sh` a worker.
- WebKit's report is marked tainted — its tab lost focus mid-session — so read its absolute
  milliseconds with that in mind. Its round-trip and keystroke figures agree with the other two
  engines, which is the check that matters.
- Incidental, and not about tiers: **one open per path, system-wide** (§5.2), so `wc /bin/sh
  /bin/sh` is a permission error. The lever above uses eight *different* files for that reason.

## T2. Size the worker pool for a system where every command needs one

`web/proc.js` is calibrated for two tier-3 binaries: `MAX_IDLE` is 2 and one worker is hired at
construction. With everything at tier 3 the shell holds one permanently, a pipeline holds one per
stage, and a four-stage pipeline would terminate and re-start workers on every run.

- Raise `MAX_IDLE` and the pre-hire count to cover a pipeline plus the shell.
- Re-decide `pool()`'s terminate-vs-keep rule against the figures from T1: a worker start is
  the cost being avoided, and it is only worth avoiding if it is large.
- Fix `broke()`'s latch: `if (!idle.length && !procs.size) workers = false` can never fire once a
  tier-3 process is permanent, so a host whose `procworker.js` will not load retries a broken
  worker on every `exec`. The probe needs an answer that does not depend on nothing running.

**Done when** the three tier-3 cases in `test/run.mjs` still pass and the pool assertions there
say what the new numbers are.

**Done.** `MAX_IDLE` is 4 — a four-stage pipeline's worth held with nothing running — and
`PRE_HIRE` is 2. `pool()` keeps its shape: the pool fills only by returning what it hired on
demand, so it self-tunes up to the peak concurrency a session reaches and the cap is where that
stops. An unbounded pool was the alternative and is what the cap was written against.

`cat /bin/sh | cat | wc` at tier 3 goes from **hired 1, reused 2, terminated 1** on every run to
**hired 0, reused 3, terminated 0** from the second run on — the first still hires the third
worker, and now keeps it. Measurable with `make bench`, whose W3 arm is that command and whose
two warm-up runs come first, and not with `test/run.mjs`, where the stages are still tier 2 until
T3.

`broke()`'s latch now asks whether *this* worker ever announced itself, which is the question the
old one was trying to ask through `!idle.length && !procs.size`. `{ k: "ready" }` was already on
the wire and already documented as meaning nothing else; `deliver()` records it, and a worker that
errors before sending it is a `procworker.js` that will not load. `test/run.mjs` has a case for it
— the second command after a broken worker runs at tier 2 and hires nobody — over a new
`net.broken` in `test/fakeworker.mjs`, since nothing had ever called `link.onerror`.

One thing found on the way: a pooled worker was pinning its finished process's memory until the
next bind, so `serveProc.step` now releases the instance and its memory on the step that ends the
process, as the trap path already did. Four idle workers would otherwise have held four dead
processes' pages.

T1's every-64th `setTimeout(drain, 0)` finding is **not** part of this, by decision: it is a change
to how the kernel worker yields, it wants a measurement of its own, and the pool does not depend
on it.

## T3. Flip the thirty-one programs

- `src/cmd/CMakeLists.txt`: tier 3 for everything but `sh`. If the list ends up being all of
  them, the default in `cmake/BraamProgram.cmake` is the better place — but leave that until
  T8, since the SDK's default is a claim about what an out-of-tree program should ask for.
- `test/run.mjs`'s `want_tier` map grows to match.

**Done when** `make run` is green, or fails only in the places T4 is about.

## T4. Rework the tests that assume only `spin` and `tail` take a worker

The driver already pumps both tiers uniformly (`test/fakesvc.mjs`'s `net.drain`), so most of the
suite should hold. Three places will not:

- The pool assertions — `net.proc.pooled()` in the tier-3 case and again before `timeout 20 spin`
  — count a pool nothing else is drawing from.
- `net.bound.length` is used to mean "something ran at tier 3"; it now means "anything ran".
- The fallback case (`net.workers = false; net.proc.dropWorkers()`) is fine while `sh` is tier 2
  and becomes T7's problem when it is not.
- Expect tick counts to drift where a command now costs a round trip per syscall; `run()` loops
  until nothing moves, so the shape holds, but literals will move.

**Done when** `make run` is green with no assertion weakened — a count that is now
"at least one" rather than "exactly one" has stopped testing the pool.

## T5. Re-measure, and decide whether to go on

T1's figures with the whole system at tier 3. If bulk I/O is acceptable and the prompt is
under a frame, skip T6.

## T6. Only if T5 says so: cut the number of round trips

Both options are expensive, and neither should be started on a guess.

- **A bigger `SYS_CHUNK`.** It is 512 because that is the allocator's top size class on both
  sides of the wire (Concept.md §8.2); raising it costs a 64 KiB span per buffer, so it is an
  allocator change as much as an ABI one.
- **Batching the replies.** The step protocol already carries a *list* of parked calls up
  (`workerOps.sysAsync`) and exactly one reply down — `_resume(token, ptr, len)` answers one
  call. Batching means the kernel coalescing the steps it owes one pid, both halves of
  `web/proc.js`, `web/procworker.js`, `test/fakeworker.mjs`, and an amendment to
  System_Calls.md. A week, and a protocol change to a thing that currently works.

## T7. Decide what happens to the shell when the workers go

This is the design question, and it is answered in Concept.md §4 before it is answered in code.
`dropWorkers()` exists so that a host that loses its workers keeps its tier-2 processes — *"The
shell is one of those, and permanent"* — and `proc.shutdown()` was split in two for it. A tier-3
shell dies there and nothing re-execs init, so the session ends with it. There is no falling a
*running* process back to tier 2: there is no state to carry over.

Three answers, in the order they are worth considering:

- **Init respawns a dead shell.** The honest one, and the only one that makes tier 3's kill
  switch mean anything for `/bin/sh`. It is work in `src/user/` — init notices its child's exit
  status and starts another — and it has to decide what the user sees.
- **`sh` is exempt from `dropWorkers`.** Cheap, and it puts a name back into a layer that was
  built not to have one.
- **Losing the workers ends the session.** Defensible, but it is a change to what §4's fallback
  promises, and the promise is that userland does not notice.

**Done when** §4 says which, in the commit that implements it.

## T8. Flip the shell

After T7 and not before.

- `src/cmd/CMakeLists.txt`, and the comment above `BRAAM_BIN_LIB_sh` — which currently states the
  reason it is tier 2 — goes or is rewritten.
- The fallback case in `test/run.mjs` needs whatever T7 decided, and a case for it.
- Consider moving the default to tier 3 in `cmake/BraamProgram.cmake` here, so the SDK's example
  and an out-of-tree program follow the system's own answer.

## T9. Documentation, in the same commits as the code

- **Concept.md §4** — the tier table's "Used for" column, which today reads *"every program, the
  shell included"* against tier 2, and §4.2's "hired from a small pool".
- **CLAUDE.md** — the process-model section, and the known gaps: *"Every command costs an
  instantiation — roughly a millisecond"* becomes a worker, and the keystroke's six ticks become
  six round trips.
- **Release_Notes.md** — a new heading, with T1 and T5's figures in it, since the whole
  argument for the change is those two numbers.
- **System_Calls.md** — only if T6 was done.

## What this does not fix

Two tier-3 fidelity losses stop being true of two programs and start being true of all of them
(Concept.md §4.3). Neither is worth blocking on, and both should be named in the release note:

- A binary that will not instantiate reads as a crash rather than a refusal — `procworker.js`
  swallows the throw and the kernel learns at the first step. A spawn that runs out of memory on
  a loaded page therefore reports a crash.
- `Sys::Now` becomes relative everywhere. Nothing calls `proc_now()` today, so this is a
  constraint on what may be written next rather than a regression.

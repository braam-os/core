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

**Done**, with T4 in the same change, and the list did end up being all of them — so the default
moved to `cmake/BraamProgram.cmake` now rather than in T8. `braam_add_program` stamps tier 3
unless a program asks for less, `src/cmd/CMakeLists.txt` holds one exception
(`set(BRAAM_BIN_TIER_sh 2)`) and passes an undefined `TIER` through the way it already passed an
undefined `LIBS`, and `want_tier` in `test/run.mjs` inverted to `{ "sh.wasm": 2 }`. Since the SDK
shares the recipe, `examples/hello` and an out-of-tree program follow the system's own answer —
which is what T8 said to consider and what §4 now states.

No C++ changed — not `sysabi.h`, not `src/proc/`, not `src/sh/`, not `exec.cpp`, not one file in
`src/cmd/` — and `bundle.bin` is the same size it was: the tier is a `u32` in a fixed-width custom
section, so a re-stamp moves no bytes.

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

**Done.** Green, and every count is still an exact literal. Four things had to move, and only the
first was foreseen here:

- **The pool literals.** `pooled()` is 1 after the first `spin 1` (8 hired by then, 7 terminated)
  and 2 before `timeout 20 spin` (13 and 11). The rule behind both is worth more than the numbers:
  the pool grows only for a pipeline wider than what is idle and shrinks only when a process is
  *killed*, so the count is a running record of what the session has killed.
- **`net.hold()` had to learn to count.** It held "the next process bound", which was unambiguous
  when two programs took workers. It is not now: a `clear` between the hold and the command takes
  it, and in `timeout 20 spin` the parent binds its own worker before the child that loops. It is
  `net.hold(n)` — the *n*-th bind from here — and the two `timeout` cases hold 2. The alternative
  was putting the path into the bind message so a test could hold by name, which is a protocol
  change for a test's convenience.
- **The broken-worker case cleared the screen with the broken worker.** `clear` is a program, so
  it was the first `exec` after `dropWorkers()` and crashed in `spin`'s place. The clear now
  happens before the tier is broken.
- **The two cases that give the tier up run last.** They did not before, and everything after them
  — the whole `/bin/sh`-as-a-program section, `less` claiming the screen, a shell process spawning
  pipelines — would have run at tier 2 while shipping at tier 3. Moving them to just before
  `exit 7` is what puts that coverage at the tier it ships at.

`net.bound.length` was left alone: it means "anything ran" now rather than "something ran at
tier 3", which is the same latch on a system where those are the same sentence. Tick counts did
not need touching — `run()` loops until nothing moves — and the suite is about the same speed.

## T5. Re-measure, and decide whether to go on

T1's figures with the whole system at tier 3. If bulk I/O is acceptable and the prompt is
under a frame, skip T6.

Note what T3 did to the harness: `bundle.bin` is now the archive with everything but the shell at
tier 3, so `bundle3nosh.bin` is a copy of it and the three arms are two. What the re-measurement
wants is the *other* twin — every program back at tier 2 — which the cmake `bench` target does
not pack, since when it was written that archive was the one being shipped. One more `--tier 2` pass
over a third staged copy is the whole of it.

**Done, and T6 is not worth starting.** The twin is packed: `bundle2.bin` is every program at
tier 2, `bundle3.bin` every program at tier 3, and `bundle.bin` — the archive that ships — is the
middle arm. `bundle3nosh.bin` is gone rather than duplicated. The three arm ids keep their T1
meanings, so the tables below read straight against T1's, whose figures are in brackets.

Measured at 0.2.47-8ca8053 on the same 8-core Mac, same method: ten timed runs after two warm-ups,
medians, three engines, six passes each. Not one of the nine arm reports is tainted and no
keystroke was dropped anywhere — so unlike T1, WebKit's column is not marked.

### The two figures, as the system ships

**A round trip still costs 34–45 µs**, which is T1's answer unmoved:

| | Blink | Gecko | WebKit |
|---|---|---|---|
| tier 2, every program | 41.8 µs (40.2) | 79.1 µs (80.8) | 10.1 µs (5.1) |
| **as shipped** | **44.9 µs** (42.3) | **38.7 µs** (33.7) | **33.7 µs** (33.7) |
| tier 3, sh included | 47.0 µs (44.4) | 40.4 µs (38.7) | 35.4 µs (38.7) |

Gecko and WebKit step `performance.now()` in whole milliseconds, so their tier-2 row is quantised
— 297 round trips into a 1 ms grid is a ±3.4 µs quantum, and WebKit's 5.1 → 10.1 is one grid step,
not a regression.

**The prompt is exactly where it was, because the shell is where it was.** Key to the last repaint
it caused, and 64 keys back to back:

| | Blink | Gecko | WebKit |
|---|---|---|---|
| tier 2, every program | 0.20 ms | 2.00 ms | 0.00 ms |
| **as shipped** | **0.20 ms** | **2.00 ms** | **0.00 ms** |
| tier 3, sh included | 0.65 ms | 2.00 ms | 3.00 ms |
| 64 keys, per key — as shipped | 0.09–0.12 ms | 0.41–0.42 ms | 0.25–0.27 ms |
| 64 keys, per key — tier-3 shell | 0.27–0.28 ms | 2.58–2.62 ms | 6.20–6.25 ms |

The shipped row is the tier-2 control's row to the clock in all three engines. Sustained typing is
0.09–0.42 ms a key, a fortieth of a frame at worst. T1's alarming figures were never the system's:
they are the tier-3-shell arm's, and in WebKit that arm got *worse* on the re-run — 6.2 ms a key
against 5.1. That is T7 and T8's number, and it is the strongest thing in this table.

### Bulk I/O, which is what the decision turns on

Medians, in milliseconds:

| | Blink | Gecko | WebKit |
|---|---|---|---|
| `wc /bin/sh` — tier 2 | 4.2 (4.2) | 9.5 (9.0) | 6.0 (6.5) |
| `wc /bin/sh` — as shipped | 11.0 (11.4) | 15.5 (16.0) | 12.0 (12.0) |
| `wc` over eight files — tier 2 | 16.6 (16.2) | 33.0 (33.0) | 9.0 (8.0) |
| `wc` over eight files — as shipped | 24.4 (24.0) | **27.0** (26.0) | 22.0 (22.0) |
| `cat /bin/sh \| cat \| wc` — tier 2 | 7.8 (7.7) | 13.0 (13.0) | 9.5 (11.5) |
| `cat /bin/sh \| cat \| wc` — as shipped | 19.7 (25.6) | 26.0 (30.0) | 19.0 (27.0) |

So the tier costs **10–13 ms** on the heaviest thing in the suite — a quarter of a megabyte through
three processes — and 6–7 ms on an 86 KB file. In Gecko `wc` over eight files is *faster* as
shipped than with everything at tier 2, for the reason below.

All of that is round trips and none of it is the worker. `true` — one spawn, one exit, no I/O —
costs the same as shipped as it does at tier 2 to within 1.5 ms in every engine (+0.2 Blink,
+1.5 Gecko, +0.0 WebKit), because the pool has a worker waiting. The tier's price is paid per
`SYS_CHUNK`, exactly where §4.4 says it is.

### What else the re-run found

- **T2's pool sizing is worth 4–8 ms on a pipeline, in every engine.** `cat | cat | wc` goes from
  T1's `hired 1, reused 2, terminated 1` to `hired 0, reused 3, terminated 0` in all three, and the
  command drops 25.6→19.7 ms in Blink, 30→26 in Gecko and 27→19 in WebKit. That is the whole of
  the improvement in the last row above; the per-round-trip cost did not move.
- **T1's every-64th `setTimeout(drain, 0)` finding has mostly retired itself.** `wc` over eight
  files takes that route 8 times at tier 2 — costing 9.5 ms in Blink, 18.5 in Gecko, 2.0 in WebKit
  — and **0 times as shipped**, because the only tier-2 process left is the shell, which does 21
  steps rather than 483. The item T2 deferred is now worth a few milliseconds of `sh`'s own
  stepping instead of the larger half of every bulk command. It is also why Gecko's shipped figure
  beats its tier-2 control.
- **The Gecko and WebKit boot flake is still there and still self-correcting**: three `selectall`
  retries across the eighteen passes, every one recovered by the harness's own reload.

### The decision

**Skip T6.** Both of its conditions are met and neither of its options is cheap. The prompt is
0.09–0.42 ms a key sustained with nothing dropped — forty to two hundred times inside a frame —
and bulk I/O costs at most 13 ms over tier 2 on the largest workload in the suite. A bigger
`SYS_CHUNK` is an allocator change (§8.2) and batched replies are a protocol change to something
that works, for a saving nothing today can perceive.

What would reopen it is a workload, not a number: something that moves megabytes rather than a
quarter of one — `curl` into a file, `grep` over a large tree — where 34–45 µs per 512 bytes is
70–90 ms a megabyte. T6 stays written down for that day.

## T6. Only if T5 says so: cut the number of round trips

**T5 says not to.** Not started, and not to be started without a workload that needs it — see the
decision at the end of T5. What follows is the shape it would take if one ever turns up.

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

**Done: init respawns.** §4 says so, in the paragraph after the fallback's. A shell that *died* —
a trap, a step that failed, an instance that would not be made — is replaced by an ordinary `exec`
of `/bin/sh`, so it lands at whatever tier the host can still give and the fallback's promise comes
out true for the shell as it is for `wc`. A shell that *exited* is not replaced: `exit` still ends
the session, and so does a status of 130, which is the kernel being disposed of rather than a shell
to start again. The bound is three deaths in quick succession, and a shell that lived longer than a
second starts the count again.

An `i32` could not carry the distinction — `exit 132` is a program's word for what 132 is the
kernel's word for — so `exec_process` gained a last defaulted `bool *died`, true on entry and false
at the one return that reports a process ending on its own terms. Init's loop re-resolves the
binary each pass (the image was moved into the instance) and clears the console foreground between
shells, or `^C` at the next prompt would reach the dead shell's pids.

One bug on the way, and it was the answer T7 would have got by default: **`dropWorkers()` never
failed the terminated worker's in-flight step**, so a tier-3 process caught mid-step left the
kernel parked for ever on a reply that was not coming. With the shell at tier 3 that is a frozen
session with no message — worse than any of the three answers, and unchosen. `kill()`'s tail is now
a shared helper both callers use. `dropWorkers` still does not set `workers` false: a host that can
still make one gets the tier back on the next `exec`.

Two cases in `test/run.mjs`, both at tier 2 since the shell does not move here, both written to
keep meaning something after T8: a held step then `dropWorkers()` — which without the fix asserts a
blank row where `[1] home $` belongs — and killing the shell's instance from the host, which
asserts the `braam: the shell died` line, a fresh prompt and a command on the new shell. It kills
pid 0, since init runs the shell with a default-constructed `Executable`, and asserts `live()`
either side so it cannot quietly stop testing anything.

**What T8 can now rely on:** losing the tier under a running `/bin/sh` costs a shell rather than
the session, whichever way the worker goes.

**And the shell has a pid.** Init ran it with a default-constructed `Executable`, so it answered to
0 — which is what `sched_spawn` returns on failure, what the terminal claims mean by "nobody", what
`SYS_WAIT_ANY` is, and what `link.pid = 0` means in `web/proc.js`. T8 puts a worker behind that pid,
so the collision was worth closing first: the shell takes init's pid now, since it is a process
inside init's task rather than a job of its own.

That turned up one thing the sentinel was hiding. `Sys::Fg` refuses a caller that does not own the
terminal, and a shell owns none of it from its second pipeline stage onwards — it lets go of the
keys before spawning, and stage one is in front by then. It passed only because
`tty_keys_owner()` and its own pid were both 0. The rule gained the clause it meant — *or what is in
front is what you put there*, recorded by the console and read through `console_fg_owner()` — and
`cat | wc` with a `^C` is the case that would have caught it. §4.3 and System_Calls.md say so.

The respawn case also moved ahead of the two tier-loss cases and grew `jobs`, `^C` at the prompt,
`^C` on a foreground of its own, and `less`: the replacement is a whole shell, asserted at the tier
the system ships rather than at the fallback.

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

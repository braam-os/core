# Braam — Milestones

The development plan and its progress. Each milestone has one objective and one acceptance
criterion; tick the boxes as work lands.

[Concept.md](Concept.md) is the specification — it says what the system is and why, and changes
only when a design decision changes. This file changes as work proceeds.
[Release_Notes.md](Release_Notes.md) explains, after the fact, why the code that landed looks
the way it does. Reasoning belongs there, not here; notes below record only how a milestone
departed from its plan.

---

## M0 — Nucleus — **done**
Freestanding build, the coroutine shim, the allocator, `Str`/`Vec`, `host_log`.
Set a binary-size budget now and track it in CI from the first commit.

- [x] `make` produces a wasm binary with the Appendix C command line
- [x] A static page loads a 4 KB wasm and logs a line to the console
- [x] Size budget recorded (32 KiB) and enforced by CI

CMake is the build system; the top-level `Makefile` is a wrapper over it (`all`, `run`, `serve`,
`clean`). `Span<T>`, `Result<T, E>` and `Option<T>` came along with `Str`/`Vec`,
since M1 needs them immediately; `String` and `HashMap` wait for M1, where the wake-token table
will shape the latter. Appendix C's command line changed in three ways — see Concept.md §C.3,
and [Release_Notes.md](Release_Notes.md) for the reasoning behind each.

## M1 — Scheduler — **done**
`Task<T>`, ready queue, wake tokens, `tick()`, `sleep_ms`.
`CancelToken` participates in every awaitable from this milestone on (Concept.md §8.1).

- [x] Two coroutines interleave sleeps in the correct order
- [x] Cancelling a sleeping task unwinds it and runs its destructors

Timers live in the kernel: there is no `host_timer` import, and `tick()`'s return value is the
only thing that schedules the host's next call — see Concept.md §3.4 and
[Release_Notes.md](Release_Notes.md). `String` and `HashMap` landed as planned; `CancelToken`
travels in `Task`'s promise rather than in every signature, and `CO_TRY` joins `TRY`, since a
plain `return` is ill-formed in a coroutine. Both acceptance criteria are checked twice: in
`tests.wasm` against a fake clock, and in the smoke test against the shipping `kernel.wasm`.

## M2 — Screen and keys — **done**
Cell grid, canvas renderer, damage rectangles, `Channel<Key>`, `OffscreenCanvas` transfer.

- [x] Typed characters appear on screen and the cursor moves
- [x] Window resize reflows and `resize(cols, rows)` reaches the kernel

`resize` returns the address of a screen descriptor, which is how the host learns the geometry
and where the cells are — Concept.md §3.4 is amended to say so. Reflow keeps the rows in use and
drops from the top; re-wrapping logical lines waits for M7's layout layer.
`Channel<T>` landed with `co_await recv()` and a non-blocking `try_send()`, but not
`co_await send()`: backpressure is M4's to define, where pipes make it testable. `init` creates
an 80×24 grid so the kernel is never screenless, and the host's first `resize` reflows the boot
banner into the measured geometry. See [Release_Notes.md](Release_Notes.md).

## M3 — Userland shell — **done**
`LineEditor` coroutine with history and editing, tokeniser, program registry, argv, exit codes.

- [x] `echo hello` prints, `help` lists registered programs
- [x] Up-arrow recalls history; a nonzero exit code is observable

Static initialisers now run: `init()` calls `__wasm_call_ctors` after `heap_init`, which is what
lets a program register itself (Concept.md §3.6) and amends the invariant CLAUDE.md stated. The
registry is a sorted intrusive list rather than a `HashMap`, since `help` needs iteration.
The tokeniser splits on whitespace and nothing else — quoting belongs with M4's grammar, next to
pipes and redirection. The exit code shows up in the prompt (`[1] $`) rather than in a
diagnostic line. `sleep` takes milliseconds. ^C abandons the line being edited; ^C interrupting a
*running* program is still M4's criterion, though the cancellation path it needs is already
tested. Programs shipped: `clear`, `echo`, `false`, `help`, `sleep`, `true`, `version`.
The size budget was raised from 32 KiB to 256 KiB; `kernel.wasm` is 28,282 bytes.
See [Release_Notes.md](Release_Notes.md).

## M4 — Streams — **done**
`Channel<Bytes>` as stdio, pipes, redirection, cancellation on `^C`.

- [x] `ls | grep foo` works
- [x] `^C` interrupts a running pipeline and returns a prompt

A pipe is a `Channel<String>`, not a `Channel<Bytes>`: `Bytes` is a `Span`, and a writer's buffer
does not outlive the handover to a reader that runs later. `ls` lists the program registry — what
`/bin` will hold — so the criterion is met literally and M5 replaces one function body. The
grammar is written whole (quoting, escaping, `|`, `<`, `>`, `>>`, `2>`, `2>>`), but a redirection
to a path is refused at setup with `no filesystem` and nothing runs; quote removal ended the
zero-copy argv borrow, so `Args` now views an owning store. The stages of a pipeline are
independent scheduler jobs alongside a tty pump rather than a child group the shell `co_await`s —
`CancelState::waiting` is a single slot, so one job cannot have two children parked at once, and
§3.6's structured concurrency is put back by hand from a destructor. The intrusive `Waiter` queue
M2's notes promised to this milestone is still not built: every pipe has one writer, so one send
token suffices, and a tripwire now fires if that stops being true. `LineEnd::Eof` also waits —
^D closes a program's stdin through the pump, and at the prompt there is nothing to exit to.
Programs added: `cat`, `grep`, `head`, `ls`, `tail`, `wc`, for thirteen. `kernel.wasm` went from
28,282 to 62,926 bytes against an unchanged 256 KiB budget. See [Release_Notes.md](Release_Notes.md).

## M5 — Filesystem — **done**
Mount table, `MemFs`, `BundleFs` from a fetched archive, `OpfsFs` with the open-file table.

- [x] Write a file, reload the page, the file is still there
- [x] `df` reports quota, usage, and persistent vs best-effort mode
- [x] With OPFS unavailable, the system boots on `MemFs` and says so

Storage reaches the host through two imports rather than one per operation — `host_fs` for the
asynchronous half and `host_fs_sync` for §5.2's sanctioned exception — which is the shape §4.3
fixes for the process ABI; Concept.md §3.4 is amended to say so, and `host_storage_read`/
`host_storage_write` are gone. An asynchronous request is a record the kernel owns *past* a
cancelled await, since the host still holds its address: the awaitable orphans it rather than
freeing it, and the reply is what reaps it. `Fs` splits by when the work can happen rather than
by what it does — naming is async, an open file is not — so a redirection opens at job setup and
every write after that is a plain call.

`BundleFs` reads one archive the worker loads beside `kernel.wasm`, not the Cache API: that API
stores `Request`/`Response` pairs and is worth having once M6's `fetch` exists to make them.
`/bin` became `BinFs`, the program registry as a read-only filesystem, so `ls /bin` still lists
the programs and `ls` itself is an ordinary directory walk. The working directory is one global
rather than per-process, because a program is handed `(Args, Stdio)` and nothing else until M8;
the shell starts in `/home`. The open-file table refuses a *second open of any kind* rather than
a second writer, because that is what an OPFS sync access handle enforces.

Criterion 1 is checked mechanically as well as by hand: the smoke test writes a file, throws the
instance away, builds a new one against the same JS-side store, and reads it back.
Programs added: `cd`, `df`, `mkdir`, `mount`, `pwd`, `rm`, `touch`, for twenty; `cat`, `grep`,
`head`, `tail` and `wc` gained file arguments and `ls` walks the mount table. `kernel.wasm` went
from 62,926 to 137,867 bytes against an unchanged 256 KiB budget.
See [Release_Notes.md](Release_Notes.md).

## M6 — Host services — **done**
`fetch`, timers, WebSocket, clipboard, the `externref` table and `JsRef`.

- [x] A `curl`-ish command fetches a URL and prints the body
- [x] A chat client works over a WebSocket
- [x] `/mnt/import` and `export` move files in and out

There is one new import, `host_svc(op, token, req, ref)`, not the `host_fetch` §3.4 sketched:
M5 fixed the style at one import per calling convention, and everything here is asynchronous,
so §2.2 still has exactly two sanctioned exceptions. The request record M5 built is now
`HostRequest` in `src/kernel/hostcall.h`, shared by both interfaces along with the orphan list
and the reaper; `web/abi.js` is the same move on the JS side. The `externref` table runs the
opposite way from §3.7's sketch — a table cannot be imported, so the kernel owns it and the
host deposits through a new `ref(slot, obj)` export, which is the sixth and only new one.

Timers needed nothing: the queue landed in M1 and there is deliberately no `host_timer`. What
was missing was a wall clock, since `host_now()` is `performance.now()`; `date` asks for one
through the service ABI. `/mnt/import` is a directory on the root `MemFs` rather than a mount,
because the picker hands over bytes. `chat` is the second place §3.6's structured concurrency
is put back by hand, and the first where the child outlives its parent — so it writes to the
screen rather than through stdout, whose pipe it does not own. `tools/wsd.mjs` is a
dependency-free broadcast server so two tabs can chat with no internet; `make serve` starts it.

`pbpaste` is the one command a browser can refuse outright: a clipboard read is only allowed
inside a user-gesture handler, and a command's request arrives after that handler has returned.
It falls back to waiting for a paste, which is a gesture needing no permission.

Programs added: `chat`, `curl`, `date`, `export`, `import`, `pbcopy`, `pbpaste`, for
twenty-seven. `kernel.wasm` went from 137,867 to 181,545 bytes against an unchanged 256 KiB
budget. See [Release_Notes.md](Release_Notes.md).

## M7 — Depth — **done**
A layout/widget layer over the cell grid (panes, a `less`, an editor), job control,
`/proc`-style introspection, an embedding API for host pages.

- [x] A full-screen editor opens, edits, and saves a file
- [x] Jobs can be backgrounded and listed

Nothing here touched the ABI: no new import, no new export, and the exact-surface assertion in
`test/run.mjs` is unchanged — which is the clearest statement that §2.2 and §3.4 are load
bearing rather than decorative. Two functions were added inside the kernel: `screen_touch`,
because the layout layer fills cells through `screen_cells()` and nothing marked them damaged,
and `sched_procs`, because a task had no name and the scheduler had nothing enumerable.

The layout layer is `src/ui/`, a fourth library beside `braam_fs` and `braam_svc`, holding
`Pane`, `FullScreen`, `TextBuf` and `TextView` and depending on the kernel alone. A pane is a
primitive an application composes its own screen out of, not a multiplexer: two jobs visible at
once needs per-pane output routing and a window manager in the shell, which is a milestone of
its own. `chat` still writes to the screen rather than into a pane, since it has no foreground
to draw in.

A full-screen program does not take the keyboard — it claims a route through the tty pump
(`KeyInput`, `InputClaim`), because `keys()` has one receiver and that receiver is the pump.
`^C` is never routed to a claimant. `less` reads its input to the end before it paints, since
`CancelState::waiting` is a single slot and no task can be parked on a pipe and the keyboard at
the same time; that same constraint is why `fg` never awaits keys at all.

Job control is `&`, a job table beside the job runtime, and `jobs`/`fg`/`kill`. There is no
`bg` and no `^Z`: stopping a running coroutine at an arbitrary point is the resume-side twin of
`CancelToken`, and it would have to reach every awaitable. `/proc` is flat — `/proc/42` is a
file, not a directory. `web/braam.js` is the embedding API, `web/embed.html` runs two kernels
on one page, and `index.html` is now sixty lines that mount one. Re-wrapping logical lines on
resize, which §3.5 had promised to this milestone, is deferred with a reason.

The embedding API was checked in a real browser rather than only in Node, which turned up two
defects older than this milestone: boot blocked on `navigator.storage.persist()`, which took
over five seconds in Firefox and never settled at all for a second instance asking concurrently;
and `build/web/` was assembled by the kernel's `POST_BUILD`, so a web-only edit left `make serve`
serving the previous copy. Both are fixed here, and Concept.md §5.3 is amended for the first.

Programs added: `edit`, `fg`, `jobs`, `kill`, `less`, for thirty-two. `kernel.wasm` went from
181,545 to 225,784 bytes against an unchanged 256 KiB budget.
See [Release_Notes.md](Release_Notes.md).

## M8 — Isolated processes — **done**
The Concept.md §4.3 ABI, per-process `WebAssembly.Instance`, per-PID import closures,
per-process memory caps, module cache, cross-boundary copies (Concept.md Appendix B).

- [x] A program runs as its own instance with a 16 MB cap and `memory.grow` fails past it
- [x] A process cannot issue a syscall on behalf of another PID
- [x] Tier selection comes from binary metadata; userland behaviour is unchanged

There is **no new import**: spawning, stepping and killing a process are three more operations
on `host_svc`, which is already the convention they want, so §2.2 still sanctions exactly two
synchronous exceptions. There are two new exports, `sys` and `sys_async` — a process's own two
imports, arriving with the pid the host bound into its closure, which is the second criterion in
one sentence: there is no argument for a pid on the calling side. §4.3 is amended in four places
and none of them is structural: memory is imported so the cap is the kernel's, `_start` takes
argv rather than argc, a reply payload starts with a status, and **the kernel never calls a
process — the host does, and never while the kernel is on the stack.**

A tier-2 program is still an ordinary scheduler job: a proxy task issues the steps and performs
the syscalls, so backpressure, `^C`, `kill`, `jobs`, `/proc` and the stage epilogue all work with
nothing added. Its destructor drops the instance, which makes a kill total rather than
cooperative — a killed process never unwinds, and its whole memory goes at once.

`wc` and `tail` moved out of the registry into `/usr/bin`, which is how the third criterion is
*checked* rather than argued: `echo 'a b' | wc`, `wc < notes` and `curl /hello.txt | wc` are M4's,
M5's and M6's own assertions, unchanged, now running an instance. `echo` and `sleep` were meant
to move too and could not: the in-wasm unit tests drive them, and `run_tests()` cannot step an
instance because stepping one means returning to the host. That constraint is now written into
§4. `hog` is new and exists to be refused: it takes everything it can and asks for one page more.

The syscall table is deliberately small — `exit`, `getpid`, `now`, `stage` synchronously;
`write`, `read`, `open`, `close` asynchronously — and every entry has a caller. Paths still
resolve against the one global cwd, so M8 isolates address space, memory and descriptors but not
the namespace a process can name. The registry went from thirty-two programs to thirty, with
three binaries beside it; `kernel.wasm` went from 225,784 to 236,872 bytes against an unchanged
256 KiB budget, and the binaries carry budgets of their own.
See [Release_Notes.md](Release_Notes.md).

## M9 — Liveness isolation
The own-worker tier: worker pool, `worker.terminate()` as `SIGKILL`, module `postMessage`.
Optionally, fuel injection as a metering alternative.

- [x] `while(1){}` in an untrusted program is killable without reloading the page
- [x] The shell stays responsive while such a program runs

**The §4.3 ABI did not change**, which was not the expectation: `sys` is synchronous and a worker
boundary has no synchronous direction. It survives because none of its four operations has to
reach the kernel — `getpid` is bound into the worker, `now` is a clock the step message carried,
`exit` rides back on the step's reply, and `stage` is the host's call and is refused with the
"no room" answer the runtime already handles. So `src/proc/` and the binaries are untouched and
the same binary runs at either tier; the kernel diff is three edits, and 93 bytes.

The kill needed no kernel code either — `^C` cancels the proxy, `~End` calls `proc_kill`, and the
host terminates a worker instead of dropping a Map entry — except for one thing that would have
leaked forever: the in-flight step has to be *failed* when the worker goes, because an abandoned
request is only reaped by `wake()` on its token.

Departures from the plan: the tier-3 protocol carries a syscall and an exit status on the step's
reply rather than as messages of their own, so a step is one message each way. The worker pool
doubles as the capability probe, which is what makes §4's "runs at tier 2 until there is a worker
to put it in" a shipped fallback. `spin` is new and exists to be un-killable by cooperation;
`tail` moved to tier 3, so M8's own `tail … | wc` assertion is now a tier-3 process feeding a
tier-2 one. Two fidelity losses are recorded in §4.3: a binary that will not instantiate reads as
a crash rather than as a refusal, and `Sys::Now` is relative. Fuel injection was not built — a
kill is what the criteria asked for, and metering is still the only way to *bound* CPU.

CI runs the whole protocol over a link with no thread in it (`test/fakeworker.mjs`), so what it
cannot prove is preemption: a looping program is modelled as a step that never comes back, which
is exactly what the kernel sees of one. The real thread was driven by hand against the shipping
`web/procworker.js`, and `terminate()` returned in 2 ms with the instance mid-loop.
See [Release_Notes.md](Release_Notes.md).

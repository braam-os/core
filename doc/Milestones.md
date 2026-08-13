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

## M6 — Host services
`fetch`, timers, WebSocket, clipboard, the `externref` table and `JsRef`.

- [ ] A `curl`-ish command fetches a URL and prints the body
- [ ] A chat client works over a WebSocket
- [ ] `/mnt/import` and `export` move files in and out

## M7 — Depth
A layout/widget layer over the cell grid (panes, a `less`, an editor), job control,
`/proc`-style introspection, an embedding API for host pages.

- [ ] A full-screen editor opens, edits, and saves a file
- [ ] Jobs can be backgrounded and listed

## M8 — Isolated processes
The Concept.md §4.3 ABI, per-process `WebAssembly.Instance`, per-PID import closures,
per-process memory caps, module cache, cross-boundary copies (Concept.md Appendix B).

- [ ] A program runs as its own instance with a 16 MB cap and `memory.grow` fails past it
- [ ] A process cannot issue a syscall on behalf of another PID
- [ ] Tier selection comes from binary metadata; userland behaviour is unchanged

## M9 — Liveness isolation
The own-worker tier: worker pool, `worker.terminate()` as `SIGKILL`, module `postMessage`.
Optionally, fuel injection as a metering alternative.

- [ ] `while(1){}` in an untrusted program is killable without reloading the page
- [ ] The shell stays responsive while such a program runs

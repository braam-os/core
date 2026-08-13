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

## M4 — Streams
`Channel<Bytes>` as stdio, pipes, redirection, cancellation on `^C`.

- [ ] `ls | grep foo` works
- [ ] `^C` interrupts a running pipeline and returns a prompt

## M5 — Filesystem
Mount table, `MemFs`, `BundleFs` from a fetched archive, `OpfsFs` with the open-file table.

- [ ] Write a file, reload the page, the file is still there
- [ ] `df` reports quota, usage, and persistent vs best-effort mode
- [ ] With OPFS unavailable, the system boots on `MemFs` and says so

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

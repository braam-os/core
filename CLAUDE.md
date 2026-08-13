# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository state

M0 (nucleus) is done: CMake build, the coroutine shim, the allocator, `Str`/`Span`/`Vec`/
`Result`/`Option`, `host_log`, a Node test harness, and CI with a size budget. M1 (scheduler)
is done: `Task<T>`, the ready and timer queues, wake tokens, `tick()`/`wake()`, `sleep_ms`,
`CancelToken`, `HashMap` and `String`. M2 (screen and keys) is done: the cell grid and its
damage rectangle, `Channel<T>`, `Key`, the `key()`/`resize()` exports, the `host_present`
import, and the canvas renderer in `web/render.js`. M3 (userland shell) is done: the
`LineEditor` coroutine, the tokeniser, the self-registering program registry, `Args`/`Stdio`,
exit codes, and seven programs in `src/prog/`. M4 (streams) is done: `co_await send()` with
`close`/`hangup`, pipes as `Channel<String>`, `Stream`/`Source` behind `Stdio`, the full shell
grammar in `src/user/parse.{h,cpp}`, the job runtime and tty pump in `src/user/job.{h,cpp}`,
and thirteen programs. M5 (filesystem) is done: `src/fs/` with the `Fs` interface, path
resolution, the mount and open-file tables, `MemFs`, `BundleFs`, `OpfsFs` and the `host_fs`/
`host_fs_sync` ABI; `BinFs` and the boot mounting in `src/user/`; working redirection; and
twenty programs. M6 (host services) is next.

**[doc/Concept.md](doc/Concept.md) is the specification.** Read it before doing anything
substantive — it carries decisions whose rationale is not recoverable from the code. It is
stable: amend it only when a design decision changes, and then in the same commit that changes
the code. Its section numbering is cited from source comments, so do not renumber.

**[doc/Milestones.md](doc/Milestones.md) is the working plan** — M0–M9 with checkbox
acceptance criteria. Tick them as work lands. This is the file that moves on an ordinary
commit; note there how a milestone departed from its plan, but put reasoning in the release
notes.

**[doc/Release_Notes.md](doc/Release_Notes.md) records why the code is the way it is**, per
milestone. Comments in the source stay terse and say *what*; the *why* goes here. Read the M0
section before touching the allocator, the coroutine shim, or the build flags — each departs
from an obvious approach for a reason stated there and nowhere else.

## What this project is

Braam is a CLI-oriented operating system that runs entirely in a browser tab: a kernel, shell,
filesystem, terminal, and programs, written in freestanding C++20 and compiled to WebAssembly,
deployable as a static site with no server and no special HTTP headers.

## Build

CMake with a toolchain file, generating Unix Makefiles — so the whole toolchain is clang, cmake,
make and node, with no ninja. The top-level `Makefile` wraps it and configures on first use:

```
make            # build kernel.wasm and tests.wasm
make run        # ctest
make serve      # serve build/web/ and open a browser
make clean      # rm -rf build
```

Overrides: `JOBS=1` for a serial build (the default is the CPU count), `GENERATOR=Ninja` if it
is installed and you want the ~30% faster build, `BUILD=<dir>` for the build tree.

`CMAKE_ARGS` passes flags to the configure step, which only happens on a fresh tree or after
`make clean` — so `make CMAKE_ARGS="-DBRAAM_WERROR=ON"` on an already-configured tree does
nothing. `-DBRAAM_WASI_SDK=<path>` relocates the SDK (default `/opt/wasi-sdk-33.0`);
`-DBRAAM_WERROR=ON` is what CI uses. The build produces `build/kernel.wasm`, a separate
`build/test/tests.wasm`, and a ready-to-serve `build/web/`.

Note that `make -jN` does **not** reach the compiler: make's jobserver descriptors do not
survive the intervening cmake process, so the wrapper passes `-j $(JOBS)` explicitly. Change
`JOBS`, not `-j`.

`/opt/wasi-sdk-33.0` is used as a clang distribution only. **Nothing from its runtime or its
headers is linked or included** — `-nostdinc++` is not optional, and `--no-default-config`
keeps its config file from injecting a sysroot. libc++'s `<coroutine>` in this SDK cannot be
used freestanding (it pulls in `<cstring>`/`<cmath>`, which need a sysroot the bare
`wasm32-unknown-unknown` target does not have). A hand-written shim over the `__builtin_coro_*`
intrinsics replaces it, at [src/kernel/coroutine.h](src/kernel/coroutine.h).

Two link flags from the original Appendix C line are deliberately **absent**, and adding either
back would be a regression: `--export-dynamic` (unreliable — exports are named individually
with `BRAAM_EXPORT`) and `--allow-undefined` (without it, an accidental libc dependency is a
link error rather than a runtime trap). See Concept.md §C.3.

Verification is per-milestone — each milestone in Milestones.md states its own acceptance
criterion — plus three CTest cases that run on every build: `smoke` asserts `kernel.wasm`'s
exact import/export surface and that it boots, `unit` runs `tests.wasm` under Node, and `size`
checks `tools/size_budget.txt`. New core code gets a case in [test/unit/](test/unit/).

Both wasm modules import the storage ABI, so both are driven with the in-memory backend in
[test/fakefs.mjs](test/fakefs.mjs). It answers from inside the import, which no browser can do
and the kernel cannot tell — `wake()` only queues a resumption. It takes its constants and its
encoders from `web/fs.js`, so the two sides of the wire cannot drift silently; keep it that way
rather than restating the format.

## Architecture invariants

These three rules answer most "how should X work?" questions, and breaking one silently is the
main way to damage the design. Full statements in Concept.md §2.

1. **Coroutines are processes; the browser event loop is the scheduler.** Every operation that
   would block becomes a `co_await`. Nothing blocks, so there is no Asyncify, no JSPI, no
   threads, no stack switching. A suspended process is a coroutine frame in a hash map.
2. **A JS import never returns data — only accepts a wake token.** Results arrive later through
   the `wake()` export. Exactly two exceptions are sanctioned: `host_now()`, and OPFS sync
   access handles once a file is open (`host_fs_sync`). A third exception needs written
   justification in Concept.md, because at three it stops being pragmatism and becomes a second
   ABI. Storage is multiplexed — one import per calling convention, not one per operation — so
   adding an operation is an enum value on each side, not a new import.
3. **The terminal is a cell grid in linear memory, not a byte stream.** No ANSI escapes, no
   VT100 emulation, no xterm.js. Colours are struct fields and cursor addressing is indexing.

Further constraints that are easy to violate by habit:

- **No exceptions, no RTTI.** Errors are `Result<T, E>`, propagated with a `TRY()` macro.
- **No `SharedArrayBuffer`**, therefore no COOP/COEP headers, therefore it hosts anywhere.
- **Every awaitable is cancellation-aware from M1 on.** `CancelToken` participates in every
  `await_suspend`. Retrofitting this later is painful, so do not defer it.
- **Coroutine frame allocation is the hot path.** The allocator is built for that workload
  first; a naive `malloc` will dominate the profile.
- **A coroutine frame past 512 bytes costs a whole 64 KiB span.** That is the allocator's top
  size class, so long-lived state belongs in a heap block the frame points at, not in the frame.
  `test_shell` guards the shell's boot cost for exactly this reason, and it is why the boot
  mounting is its own coroutine and why `FS_BLOCK` is 512 rather than a rounder number.
- **A host request may outlive the coroutine that issued it.** Anything whose address crosses to
  JS and comes back later — every `FsCall` — must be a heap record the kernel keeps alive past a
  cancelled await, never a buffer in the awaiting frame. `wake()` on an unclaimed token is what
  reaps one; that is why `sched_wake` returns a bool.
- **Never `new` anything.** `operator new` returns null on failure and `-fno-exceptions` means
  the expression would construct at address zero. Use `heap_new`/`heap_delete` from `alloc.h`.
- **Every awaiter deregisters in its destructor.** `sched_unwait` from `~Awaiter` is what makes
  destroying a suspended frame safe rather than a dangling `Waiter *` in the wake table. An
  awaitable that parks and has no destructor is a use-after-free waiting to happen.
- **`memory.grow` detaches the `ArrayBuffer`**, killing cached `Uint8Array` views. Route JS-side
  access through a `view()` accessor that re-derives after growth.
- **A namespace-scope global must be trivially destructible.** A non-trivial destructor pulls in
  `__cxa_atexit`, which nothing provides — a link error, deliberately. Either make the state a
  POD (`Heap`, `Screen`, `Channel`, the program registry's list head) or put it behind a pointer
  built on first use (`Sched`). Constructors, on the other hand, *do* run: since M3, `init()`
  calls `__wasm_call_ctors()` itself, which is what builds the program registry. It calls it
  **after** `heap_init`, so a constructor may allocate; nothing may run before the heap exists.

## Process model

Isolation is tiered by trust, and `exec` picks a tier from binary metadata so userland does not
notice (Concept.md §4):

- **Kernel applet** — in-kernel coroutine, no isolation, no overhead. This is the only tier that
  exists through M7, which is why `/bin` is `BinFs` over the program registry rather than a
  directory of binaries, and why the working directory is one global rather than per-process.
- **Separate instance, shared worker** (M8) — address-space, capability and memory-cap isolation.
- **Separate instance, own worker** (M9) — adds a real kill switch, since wasm cannot be
  preempted.

The kernel↔process ABI (Concept.md §4.3) is already fixed even though tiers 2 and 3 are not
built, so that M0–M7 work does not have to be unpicked later. Do not design around its absence.

Since M4 a pipeline's stages are independent scheduler jobs rather than a child group the shell
`co_await`s: `CancelState::waiting` is a single slot, so one job cannot have two children parked
at once. §3.6's structured concurrency is put back by hand, from a destructor in `run_line`'s
frame. A real child-group awaitable needs intrusive queue links inside `Waiter` first — the same
work a channel with two blocked senders would need, which `Channel::park_sender` panics on today
rather than losing a wakeup quietly.

## Conventions

- **Keep comments in code and scripts terse.** A comment says what a thing is, not why the
  design is the way it is. Reasoning, alternatives and trade-offs belong in
  [doc/Release_Notes.md](doc/Release_Notes.md) or another document, where they can be read as
  prose and revised in one place.
- Commits: no `Co-Authored-By` trailer, no generated-with footer. Commit only when asked.
- Layout: `src/kernel/`, `src/fs/` (paths, the VFS, the filesystems, the host storage ABI),
  `src/user/` (line editor, grammar, job runtime, shell, `BinFs`, boot), `src/prog/` (one
  self-registering file per program), `test/unit/`, `web/`, `bundle/`, `tools/`, `cmake/`.
  Concept.md §7. `braam_fs` sits between the kernel and userland and must not depend upwards:
  anything needing the program registry belongs in `src/user/`, which is why `BinFs` lives
  there.
- **`src/prog/` is an `OBJECT` library, not `STATIC`.** Nothing references those translation
  units by name — they reach the link only through their static-init registrars, and
  `--gc-sections` never extracts an unreferenced archive member. As an archive it would link
  cleanly and produce an empty registry, silently. `test_prog` asserts the exact program count
  for that reason.
- Exports are declared with `BRAAM_EXPORT("name")`, imports with `BRAAM_IMPORT("name")` — never
  by linker flag. Either one changes the ABI, so update the expected surface in
  [test/run.mjs](test/run.mjs) in the same commit.
- `.clang-format` at the root is authoritative: 4-space indent, 100 columns. Types are
  `PascalCase`, functions and variables `snake_case`, constants `SCREAMING_SNAKE`.

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository state

M0 (nucleus) is done: CMake build, the coroutine shim, the allocator, `Str`/`Span`/`Vec`/
`Result`/`Option`, `host_log`, a Node test harness, and CI with a size budget. M1 (scheduler)
is next.

**[doc/Concept.md](doc/Concept.md) is the specification and the working plan.** Read it before
doing anything substantive — it carries decisions whose rationale is not recoverable from the
code. It is also a workbook: §6 holds milestones M0–M9 with checkbox acceptance criteria. Tick
them as work lands, and when a design decision changes, amend Concept.md in the same commit
that changes the code.

**[doc/Release_Notes.md](doc/Release_Notes.md) records why the code is the way it is**, per
milestone. Comments in the source stay terse and say *what*; the *why* goes here. Read the M0
section before touching the allocator, the coroutine shim, or the build flags — each departs
from an obvious approach for a reason stated there and nowhere else.

## What this project is

Braam is a CLI-oriented operating system that runs entirely in a browser tab: a kernel, shell,
filesystem, terminal, and programs, written in freestanding C++20 and compiled to WebAssembly,
deployable as a static site with no server and no special HTTP headers.

## Build

CMake with a toolchain file; Ninja is the tested generator. The top-level `Makefile` wraps it
and configures on first use:

```
make            # build kernel.wasm and tests.wasm
make run        # ctest
make serve      # serve build/web/ and open a browser
make clean      # rm -rf build
```

The underlying commands, when a flag needs passing:

```
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-unknown-unknown.cmake
cmake --build build
ctest --test-dir build --output-on-failure
```

`-DBRAAM_WASI_SDK=<path>` relocates the SDK (default `/opt/wasi-sdk-33.0`); `-DBRAAM_WERROR=ON`
is what CI uses. Both reach the Makefile through `make CMAKE_ARGS=...`, which only takes effect
on the configure step — after `make clean`, or on a fresh tree. The build produces
`build/kernel.wasm`, a separate `build/test/tests.wasm`, and a ready-to-serve `build/web/`.

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

Verification is per-milestone — each milestone in Concept.md §6 states its own acceptance
criterion — plus three CTest cases that run on every build: `smoke` asserts `kernel.wasm`'s
exact import/export surface and that it boots, `unit` runs `tests.wasm` under Node, and `size`
checks `tools/size_budget.txt`. New core code gets a case in [test/unit/](test/unit/).

## Architecture invariants

These three rules answer most "how should X work?" questions, and breaking one silently is the
main way to damage the design. Full statements in Concept.md §2.

1. **Coroutines are processes; the browser event loop is the scheduler.** Every operation that
   would block becomes a `co_await`. Nothing blocks, so there is no Asyncify, no JSPI, no
   threads, no stack switching. A suspended process is a coroutine frame in a hash map.
2. **A JS import never returns data — only accepts a wake token.** Results arrive later through
   the `wake()` export. Exactly two exceptions are sanctioned: `host_now()`, and OPFS sync
   access handles once a file is open. A third exception needs written justification in
   Concept.md, because at three it stops being pragmatism and becomes a second ABI.
3. **The terminal is a cell grid in linear memory, not a byte stream.** No ANSI escapes, no
   VT100 emulation, no xterm.js. Colours are struct fields and cursor addressing is indexing.

Further constraints that are easy to violate by habit:

- **No exceptions, no RTTI.** Errors are `Result<T, E>`, propagated with a `TRY()` macro.
- **No `SharedArrayBuffer`**, therefore no COOP/COEP headers, therefore it hosts anywhere.
- **Every awaitable is cancellation-aware from M1 on.** `CancelToken` participates in every
  `await_suspend`. Retrofitting this later is painful, so do not defer it.
- **Coroutine frame allocation is the hot path.** The allocator is built for that workload
  first; a naive `malloc` will dominate the profile.
- **`memory.grow` detaches the `ArrayBuffer`**, killing cached `Uint8Array` views. Route JS-side
  access through a `view()` accessor that re-derives after growth.

## Process model

Isolation is tiered by trust, and `exec` picks a tier from binary metadata so userland does not
notice (Concept.md §4):

- **Kernel applet** — in-kernel coroutine, no isolation, no overhead. This is the only tier that
  exists through M7.
- **Separate instance, shared worker** (M8) — address-space, capability and memory-cap isolation.
- **Separate instance, own worker** (M9) — adds a real kill switch, since wasm cannot be
  preempted.

The kernel↔process ABI (Concept.md §4.3) is already fixed even though tiers 2 and 3 are not
built, so that M0–M7 work does not have to be unpicked later. Do not design around its absence.

## Conventions

- **Keep comments in code and scripts terse.** A comment says what a thing is, not why the
  design is the way it is. Reasoning, alternatives and trade-offs belong in
  [doc/Release_Notes.md](doc/Release_Notes.md) or another document, where they can be read as
  prose and revised in one place.
- Commits: no `Co-Authored-By` trailer, no generated-with footer. Commit only when asked.
- Layout: `src/kernel/`, `test/unit/`, `web/`, `tools/`, `cmake/`; `src/fs/`, `src/prog/` (one
  self-registering file per program) and `src/user/` arrive with their milestones. Concept.md §7.
- Exports are declared with `BRAAM_EXPORT("name")`, imports with `BRAAM_IMPORT("name")` — never
  by linker flag. Either one changes the ABI, so update the expected surface in
  [test/run.mjs](test/run.mjs) in the same commit.
- `.clang-format` at the root is authoritative: 4-space indent, 100 columns. Types are
  `PascalCase`, functions and variables `snake_case`, constants `SCREAMING_SNAKE`.

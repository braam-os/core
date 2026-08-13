# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository state

Pre-implementation. The repo contains `LICENSE`, this file, and [doc/Concept.md](doc/Concept.md).
There is no source code, no build system, and no tests yet; M0 has not been started.

**[doc/Concept.md](doc/Concept.md) is the specification and the working plan.** Read it before
doing anything substantive — it carries decisions and their rationale that are not recoverable
from the code, because there is no code. It is also a workbook: §6 holds milestones M0–M9 with
checkbox acceptance criteria. Tick them as work lands, and when a design decision changes,
amend Concept.md in the same commit that changes the code.

## What this project is

Braam is a CLI-oriented operating system that runs entirely in a browser tab: a kernel, shell,
filesystem, terminal, and programs, written in freestanding C++20 and compiled to WebAssembly,
deployable as a static site with no server and no special HTTP headers.

## Build

No build system exists yet; creating one is part of M0. The compile command line below is
verified working on this machine and is the starting point (Concept.md Appendix C):

```
/opt/wasi-sdk-33.0/bin/clang++ \
    --target=wasm32-unknown-unknown \
    -std=c++20 -Os \
    -nostdlib -nostdinc++ \
    -fno-exceptions -fno-rtti -fno-threadsafe-statics \
    -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined
```

`/opt/wasi-sdk-33.0` is used as a clang distribution only. **Nothing from its runtime or its
headers is linked or included** — `-nostdinc++` is not optional. libc++'s `<coroutine>` in this
SDK cannot be used freestanding (it pulls in `<cstring>`/`<cmath>`, which need a sysroot the
bare `wasm32-unknown-unknown` target does not have). A hand-written shim over the
`__builtin_coro_*` intrinsics replaces it, at `src/kernel/coroutine.h`.

Verification is currently per-milestone: each milestone in Concept.md §6 states its own
acceptance criterion. A test harness does not exist yet.

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
- Layout proposed for M0: `src/kernel/`, `src/fs/`, `src/prog/` (one self-registering file per
  program), `src/user/`, `web/`, `tools/`. See Concept.md §7.

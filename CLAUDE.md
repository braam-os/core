# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with
code in this repository.

## What this project is

Braam is a CLI operating system that runs entirely in a browser tab: kernel,
shell, filesystem, terminal and programs, written from scratch in freestanding
C++20, compiled to wasm32, deployable as a static site with no server and no
special HTTP headers. No libc, no Emscripten, no `xterm.js` — nothing is linked
that is not in this tree.

Four things must never regress: `kernel.wasm` under its 256 KiB budget, the boot
archive's staging tree under 1 MiB, the wasm ABI of six imports and nine
exports, and the three passing CTest cases.

## Documents

- **[doc/Concept.md](doc/Concept.md) is the specification.** Read it before
  anything substantive, and amend it in the same commit as the code. Its section
  numbers are cited from source comments — amend a section, never renumber it.
  `§n` below refers to it.
- **[doc/Release_Notes.md](doc/Release_Notes.md) is where the *why* goes** —
  appended under a new heading, never by rewriting an old one. It also holds
  M0–M9's objectives and acceptance criteria, which are live constraints; read
  its M0 section before touching the allocator, the coroutine shim or the build
  flags.
- **[doc/System_Calls.md](doc/System_Calls.md)** derives the kernel↔process
  mechanism (§4.3 is normative). Changes to `src/proc/`, `src/user/`,
  [src/kernel/sysabi.h](src/kernel/sysabi.h) or [web/proc.js](web/proc.js) must
  keep it true.
- **[doc/Shell.md](doc/Shell.md)** is the `/bin/sh` manual,
  **[doc/Programming_Manual.md](doc/Programming_Manual.md)** the SDK guide, and
  **[doc/Package_Management.md](doc/Package_Management.md)** the policy a
  package manager must satisfy, with
  **[doc/Package_Format.md](doc/Package_Format.md)** the grammars written to
  satisfy it (`/bin/pkg` is a skeleton — a subcommand table and nothing behind
  it; the rest is [src/cmd/pkg/TODO.md](src/cmd/pkg/TODO.md)).

## Build

CMake with a toolchain file, Unix Makefiles; clang, cmake, make, node, python3.
The top-level `Makefile` wraps it and configures on first use:

```
make            # kernel.wasm, the /bin binaries, tests.wasm, build/web/
make run        # ctest
make serve      # serve build/web/ on :8080, chat's wsd on :8081, open a browser
make install    # the SDK, to /usr/local if writable else ~/.local
make release    # pack build/web/ and the SDK as build/*.zip
make clean
```

- Overrides: `JOBS`, `GENERATOR=Ninja`, `BUILD=<dir>`, `PREFIX=<dir>`.
  **`make -jN` does not reach the compiler** (the jobserver does not survive the
  cmake process) — set `JOBS`. `CMAKE_ARGS` reaches only the configure step.
- A single test: `ctest --test-dir build -R unit --output-on-failure`, with
  `smoke`, `unit` and `size` the three names. The wasm suite has no filter of
  its own; run one case by building `tests` and reading the harness output.
- The always-run `web` target uses `copy_directory`, which never deletes — cut a
  release from a clean tree.
- Version = `BRAAM_VERSION_BASE` ([src/kernel/version.h](src/kernel/version.h),
  hand-edited) + commit count + short hash. `tools/version.py` is the one
  implementation and runs at *build* time; `tools/release.py` imports it.
- `braam_add_program(NAME … SOURCES … [LIBS])` in
  [cmake/BraamProgram.cmake](cmake/BraamProgram.cmake) is shared by `src/cmd/`
  and the installed SDK, so an out-of-tree program is built exactly as these
  are. `examples/hello/` is a build target for that reason.
- `-Wall -Wextra -Wshadow` with `BRAAM_WERROR` **ON by default**; the tree is
  warning-clean. `-DBRAAM_WERROR=OFF` is for bisecting only.

### Toolchain

- Clang only, and **nothing from its runtime or headers is linked or included**:
  `-nostdinc++` is mandatory, `--no-default-config` keeps a config file from
  injecting a sysroot. libc++'s `<coroutine>` is unusable freestanding;
  [src/kernel/coroutine.h](src/kernel/coroutine.h) shims `__builtin_coro_*`.
  There is no compiler-rt for `wasm32-unknown-unknown`.
- The toolchain file probes `/usr/local/opt/llvm` and `/opt/homebrew/opt/llvm`
  and fails at configure time naming the missing tool.
- Wasm features are named explicitly, not taken from the default CPU:
  `-mreference-types` (else `__externref_t` is unknown), `-mbulk-memory` (else
  `memcpy` is undefined), `-msign-ext -mmutable-globals -mnontrapping-fptoint`.
- Two link flags are deliberately **absent**; re-adding either is a regression
  (§C.3): `--export-dynamic` (use `BRAAM_EXPORT`) and `--allow-undefined`
  (without it an accidental libc dependency is a link error).
- `MinSizeRel` comes from the toolchain file and is not optional: at `-O0` a
  freestanding build needs libcalls nothing provides.

### Verification

- `smoke` — [test/run.mjs](test/run.mjs) under Node: asserts the kernel's exact
  imports (`host.fs`, `host.fs_sync`, `host.log`, `host.now`, `host.present`,
  `host.svc`) and exports (`init`, `key`, `memory`, `ref`, `resize`, `sys`,
  `sys_async`, `tick`, `wake`), every binary's surface and `braam` section, that
  the kernel boots to a prompt, and that `rootfs/share/help` matches the builtin
  table and the archive's `bin/`.
- `unit` — `tests.wasm` under Node. New core code gets a case in
  [test/unit/](test/unit/) and a line in
  [test/CMakeLists.txt](test/CMakeLists.txt).
- `size` — `tools/size_budget.txt`. Raising a number is a deliberate act.

Both wasm modules are driven by the in-memory backends
[test/fakefs.mjs](test/fakefs.mjs) and [test/fakesvc.mjs](test/fakesvc.mjs),
which answer from inside the import and take their constants, encoders and
archive unpacker from `web/fs.js`, `web/svc.js`, `web/abi.js` — do not restate
the format. `tools/wsd.mjs` is a real WebSocket server.

**The in-wasm suite cannot run a program**, and the shell is one. `test/unit/`
reaches everything *below* a program; the pure shell sources (`parse.cpp`,
`tokenize.cpp`, `expand.cpp`, `match.cpp`, `cond.cpp`) and `proc/opt.cpp`,
`proc/time.cpp` are compiled straight into the suite rather than linked, so a
syscall in any of them is a link error. Anything needing a program to run
belongs in `test/run.mjs`.

## Architecture invariants

Stated in full in §2. Violating one of these is a design change to argue in
Concept.md first, not a patch.

1. **Coroutines are processes; the browser event loop is the scheduler.**
   Everything blocking is a `co_await`. No Asyncify, no JSPI, no threads, no
   stack switching. A suspended process is a coroutine frame in a hash map.
2. **A JS import never returns data — only accepts a wake token.** Results
   arrive through `wake()`. Two sanctioned exceptions: `host_now()` and OPFS
   sync access handles once a file is open (`host_fs_sync`); a third needs
   written justification in Concept.md. Storage and host services are
   multiplexed — one import per calling convention — so a new operation is an
   enum value on each side, never a new import.
3. **The terminal is a cell grid in linear memory, not a byte stream.** No ANSI
   escapes, no VT100. Colours are struct fields, cursor addressing is indexing.
   Mouse selection lives on the page and in `web/render.js`; there is no mouse
   anywhere in the ABI (§3.5).

Further constraints, easy to violate by habit:

- **No exceptions, no RTTI.** Errors are `Result<T, E>`, propagated with `TRY()`
  (a statement expression — hence `CMAKE_CXX_EXTENSIONS ON`).
- **No `SharedArrayBuffer`**, hence no COOP/COEP headers, hence it hosts
  anywhere.
- **Every awaitable is cancellation-aware**; `CancelToken` participates in every
  `await_suspend`, and **every awaiter deregisters in its destructor**
  (`sched_unwait` from `~Awaiter`).
- **Coroutine frame allocation is the hot path.** A frame past 512 bytes costs a
  whole 64 KiB span; long-lived state belongs in a heap block the frame points
  at. `FS_BLOCK` is 512 for the same reason.
- **A host request may outlive the coroutine that issued it.** Anything whose
  address crosses to JS must be a heap record that survives a cancelled await,
  never a frame buffer; `wake()` on an unclaimed token reaps one, which is why
  `sched_wake` returns a bool. A slot the host deposits into belongs to the
  record: `reserve_ref()`, not a `JsRef` in the frame.
- **The externref table is the kernel's; JS never indexes it.** The host
  deposits through the `ref` export and receives objects as `host_svc`
  arguments.
- **Never `new` anything** — `operator new` returns null on failure and
  `-fno-exceptions` then constructs at address zero. Use
  `heap_new`/`heap_delete`.
- **A namespace-scope global must be trivially destructible**; a non-trivial
  destructor pulls in `__cxa_atexit`. Make it a POD or hide it behind a pointer
  built on first use (`Sched`). Constructors *do* run: `init()` calls
  `__wasm_call_ctors()` **after** `heap_init`, so a constructor may allocate.
- **A descriptor is held for the length of a syscall.** `Handle` is refcounted:
  `Close` frees the number and shuts what is behind it at once, while the block
  and its externref slot wait for the last call. Whoever may still touch a
  `Stdio` holds a reference to what is behind it (`Stdio::hold`/`owner`), and a
  syscall server takes a counted `ProcRef` **by value as a coroutine parameter**
  so the copy outlives the body's locals.
- **`memory.grow` detaches the `ArrayBuffer`**, killing cached `Uint8Array`
  views. Route JS-side access through a `view()` accessor.
- **A process binary shares headers with the kernel, not code.** `src/proc/`
  links `alloc.cpp`, `result.cpp`, `text.cpp`, `fs/path.cpp` and `braam_ui` and
  nothing else from the kernel's trees; `test/run.mjs` asserts each binary's
  import list. Hence `panic` is declared in `host.h`, defined once per binary,
  and takes `(ptr, len)` rather than a `Str`.

### Keyboard, foreground and claims

- **One receiver per `Channel`; the keyboard's is the console pump** —
  permanent, spawned by init ([src/user/console.h](src/user/console.h)). A
  program claims a route through it (`KeyInput` in
  [src/user/tty.h](src/user/tty.h)) rather than receiving on `keys()`, which
  would displace the pump silently and lose `^C`. `CancelState::waiting` is one
  slot, so no task can be parked on a pipe and on the keyboard at once.
- **`^C` cancels the foreground if there is one, and reaches the claimant if
  there is not.** The foreground is a set of pids armed with `Sys::Fg`; a shell
  arms its stages before it waits and arms nothing at a prompt. **The foreground
  belongs to whoever armed it** — `Sys::Fg`'s fourth authorisation clause, the
  only one that lets a shell arm a *pipeline* (§4.3).
- Each route (keys, screen) has **one holder; a second claim is `Err(Perm)`**. A
  claim clears its route only if it is still the holder, so parent and child may
  die in either order.
- **0 is not a pid.** It is `sched_spawn`'s failure return, "nobody" for the tty
  owners, `SYS_WAIT_ANY`, `Fg(0)`, and `link.pid = 0` in `web/proc.js`.
- `Sys::Spawn` **moves** a descriptor into the child rather than duplicating it,
  and one a syscall of the parent is parked on cannot be moved at all. A second
  concurrent use of a descriptor in the same direction is `Err(Perm)`.

## Process model

Every program is a binary; there is no in-kernel program, no registry, and no
way to write one (§4). A command word resolves as **function, then builtin, then
`PATH`**, and only the last costs a process. A file is a program when it carries
the `braam` section, otherwise when it begins `#!` and names an absolute
interpreter, which `exec_resolve` chases exactly once.

- **A shell builtin** ([src/cmd/sh/builtin/](src/cmd/sh/builtin/), twenty-six in
  `table.cpp`, plus shell functions) either touches the shell *process's* own
  state (cwd, jobs, variables, options, traps, loop) — **or its whole cost is
  the spawn**, which is `test`, `[`, `:`, `echo`, `true`, `false` and nothing
  else. The first kind has no file; the second keeps its file in `/bin`, since a
  builtin shadows the name at a prompt and not everywhere. A builtin runs **in
  its turn rather than alongside**, so it must buffer its output and write it
  once or it fills an eight-slot pipe with nobody to drain it.
- **Everything else is a process in a worker of its own** — address-space,
  capability, descriptor and memory-cap isolation plus a real kill switch.
  `braam_add_program` arranges it unasked and the `braam` section carries no
  placement word: it is what a program *is*, not what one asks for.
- **`PATH` is searched by the kernel, not the shell** — `exec_resolve` in
  [src/user/exec.cpp](src/user/exec.cpp) reads it out of the env blob the spawn
  carries. A candidate that is not a program does not shadow one further along,
  and a search that found only those is `Err(Invalid)` (126), not
  `Err(NotFound)` (127).
- **A host with no worker to give is waited out, not worked around** —
  `Error::Again`, `spawn_process` backs off, and `^C` abandons the await.
- **A process that loses its worker dies with it, and init replaces the shell**
  — when it *died*, not when it *exited* (`exec_process`'s `bool *died`),
  bounded at three deaths in quick succession. Whoever takes a worker away
  (`kill()`, `dropWorkers()`) **must fail the in-flight step**, or the kernel
  parks for ever.

The kernel↔process ABI is §4.3 and `src/kernel/sysabi.h`; both ends include the
header. Load-bearing rules:

- **The kernel never calls a process, and the host never calls one while the
  kernel is on the stack.** A step is a `postMessage`; syscalls go the other way
  and re-enter the kernel at top level.
- **A process's pid is written into its import closure, not passed** — that is
  the whole of "a process cannot issue a syscall on behalf of another pid".
- **A process may have several syscalls outstanding; the step says which one it
  answers.** `PROC_TASKS` is 8 process-side; kernel-side each parked call is a
  `Call` record with its own staging block and scheduler job, and the resume
  token rides in the step request's `flags`.
- **A process's children are cancelled by its destructor** — §3.6's structured
  concurrency by hand. A child is an ordinary scheduler job, so `^C`, `kill`,
  `jobs` and `/proc` reach it.
- **A pid is reused, but never while something still names it**
  (`sched_pid_hold`/`sched_pid_drop`; a third holder means adding a hold).
  `SYS_PID_MAX` is 999999 and is the boundary between the two id spaces: tasks
  the kernel runs for itself are named above it, are absent from `sched_procs`,
  and `Wait`/`Kill`/`Fg` cannot name them.
- **A process ends when its root task returns**, whatever the others are doing.
- **Both halves of the step protocol live in [web/proc.js](web/proc.js)** —
  `serveProc` the process's side, `makeProc` the host's; `web/procworker.js` and
  `test/fakeworker.mjs` are wiring around them.

A pipeline's stages are independent scheduler jobs rather than a child group the
shell `co_await`s, because `CancelState::waiting` is a single slot; §3.6's
structured concurrency is put back by hand from a destructor in `run_line`'s
frame. A cancelled child does not unwind until the scheduler resumes it, so it
must touch nothing the parent owns.

## Known gaps

Much of what looks missing is absent on purpose, each with a reason in
Release_Notes.md — no `bg`/`^Z`, no per-process root, no file permissions, no
hard links, no CPU metering, no directory mtime, no `setenv`, a rename that is
sometimes a copy, one program on the screen at a time, 512 rows of scrollback,
globbing in argv words only, a function that is not a scope, `( … )` that
isolates state and not memory. **None of these is a bug**; check
Release_Notes.md before "fixing" one, and adding one back is a design change to
argue in Concept.md first.

## Conventions

- **Comments say *what*, never *why*, and stay terse.** Reasoning goes in
  [doc/Release_Notes.md](doc/Release_Notes.md). Commit subjects are short, with
  a line or two of body at most; commit only when asked.
- Layout (§7): `src/kernel/`; `src/fs/` (paths, VFS, filesystems, host storage
  ABI); `src/svc/` (fetch, WebSocket, clipboard, file transfer, clock, process
  operations); `src/ui/` (layout over a `Grid`: `Pane`, `TextBuf`, `TextView`);
  `src/user/` (exec and the syscall dispatcher, console, pipes, `ProcFs`, boot
  and init); `src/proc/` (a process binary's runtime); `src/cmd/` (one file per
  program, bar `src/cmd/pkg/` and `src/cmd/sh/`).
- `braam_fs` and `braam_svc` are siblings above the kernel and below userland:
  no upward dependency and none on each other; anything needing the scheduler or
  the screen belongs in `src/user/` (hence `ProcFs`). **`braam_sh` links
  `braam_proc`**, so nothing in it may reach a kernel header that pulls in the
  scheduler. **`braam_ui` links `braam_flags` alone** and the kernel does not
  link it; keep it clear of the VFS, the screen and every host import.
- **The builtin table is an explicit array and must stay one.** `--gc-sections`
  never extracts an unreferenced archive member, so a self-registering builtin
  would be dropped silently.
- **A new program or builtin updates `rootfs/share/help` in the same commit.**
  That document is the whole of `help` (`/bin/help` is `#!/bin/sh` over `less
  /share/help`), nothing notices at run time when it goes stale, and `smoke`
  fails on a forgotten line.
- Exports are declared with `BRAAM_EXPORT("name")`, imports with
  `BRAAM_IMPORT("name")` — never by linker flag. Either changes the ABI: update
  the expected surface in [test/run.mjs](test/run.mjs) in the same commit.
- `.clang-format` at the root is authoritative: 4-space indent, 100 columns.
  Types `PascalCase`, functions and variables `snake_case`, constants
  `SCREAMING_SNAKE`.
- Markdown wraps at 80 columns (`.editorconfig`). Rewrap prose and list items;
  leave tables, code blocks and headings alone.

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository state

**The plan is finished: M0–M9 are all done.** Since then the kernel applet has been retired:
every program is a binary, `src/prog/` and the program registry are gone, and twenty-nine
programs live in `src/cmd/` beside six shell builtins in `src/user/builtin/`. `kernel.wasm` is
168,804 bytes against a 256 KiB budget and `bundle.bin` is 379 KB; the wasm ABI is still six
imports and nine exports, and the three CTest cases pass. Work here is change to a working
system, so the bar is that nothing above regresses, and Milestones.md is history rather than a
to-do list.

What each milestone left behind, since the layout still reflects it:

M0 (nucleus) is done: CMake build, the coroutine shim, the allocator, `Str`/`Span`/`Vec`/
`Result`/`Option`, `host_log`, a Node test harness, and CI with a size budget. M1 (scheduler)
is done: `Task<T>`, the ready and timer queues, wake tokens, `tick()`/`wake()`, `sleep_ms`,
`CancelToken`, `HashMap` and `String`. M2 (screen and keys) is done: the cell grid and its
damage rectangle, `Channel<T>`, `Key`, the `key()`/`resize()` exports, the `host_present`
import, and the canvas renderer in `web/render.js`. M3 (userland shell) is done: the
`LineEditor` coroutine, the tokeniser, a self-registering program registry (since removed),
`Args`/`Stdio`, exit codes, and seven programs. M4 (streams) is done: `co_await send()` with
`close`/`hangup`, pipes as `Channel<String>`, `Stream`/`Source` behind `Stdio`, the full shell
grammar in `src/user/parse.{h,cpp}`, the job runtime and tty pump in `src/user/job.{h,cpp}`,
and thirteen programs. M5 (filesystem) is done: `src/fs/` with the `Fs` interface, path
resolution, the mount and open-file tables, `MemFs`, `BundleFs`, `OpfsFs` and the `host_fs`/
`host_fs_sync` ABI; `BinFs` (since removed) and the boot mounting in `src/user/`; redirection; and
twenty programs. M6 (host services) is done: the `externref` table and `JsRef` in
`src/kernel/jsref.h`, the generic request record in `src/kernel/hostcall.h` shared by both
asynchronous imports, the `host_svc` ABI and `src/svc/` (fetch, WebSocket, clipboard, file
transfer, wall clock), the `ref` export, the page-side relay in `web/`, `tools/wsd.mjs`, and
twenty-seven programs. M7 (depth) is done: the layout layer in `src/ui/` (`Pane`, `FullScreen`,
`TextBuf`, `TextView`) — `FullScreen` has since moved to `src/user/tty.h` and the rest onto a
`Grid` — the keyboard claims in `src/user/tty.h`
routed by the tty pump, `less` and `edit`, `&` with the job table in `src/user/job.{h,cpp}` and
`jobs`/`fg`/`kill`, `ProcFs` on `/proc` with `sched_procs` under it, the `web/braam.js`
embedding API, and thirty-two programs — all with no change to the wasm ABI. M8 (isolated
processes) is done: the §4.3 process ABI in `src/kernel/sysabi.h`, the process-side runtime in
`src/proc/`, binaries in `src/cmd/` stamped by `tools/stamp.py` and packed into `/bin`,
`exec` and the syscall dispatcher in `src/user/exec.{h,cpp}`, the three process operations on
`host_svc` in `src/svc/proc.{h,cpp}`, the per-pid import closure and module cache in
`web/proc.js`, and two new exports — `sys` and `sys_async` — with no new import. M9 (liveness
isolation) is done: the own-worker tier, with **no change to the §4.3 process ABI** — the same
binary runs at tier 2 or tier 3, because each synchronous syscall is answerable inside the
process's own worker. Both halves of the kernel↔process-worker protocol live in `web/proc.js`,
`web/procworker.js` is its wiring, the worker pool doubles as the capability probe behind §4's
tier-2 fallback, and `test/fakeworker.mjs` runs the whole protocol in CI over a link with no
thread in it. `tail` and `spin` run at tier 3.

**After M9, one program model.** The applet tier is retired, in the change
[doc/Release_Notes.md](doc/Release_Notes.md) opens with: `src/prog/` and the registry deleted,
every program a binary in `src/cmd/`, `cd`/`fg`/`jobs`/`kill`/`help`/`exit` moved into
`src/user/builtin/` as true shell builtins with no file behind them, `BinFs` and `/usr` gone in
favour of `/bin` and `/share` as two views of the one bundle, the §4.3 syscall table roughly
tripled to meet what the applets used to reach for directly, `src/ui/` turned into a library over
a `Grid` that a process links, and the step protocol given a token so a process can have several
calls outstanding at once. `PROC_ABI` is 2.

**[doc/Concept.md](doc/Concept.md) is the specification.** Read it before doing anything
substantive — it carries decisions whose rationale is not recoverable from the code. It is
stable: amend it only when a design decision changes, and then in the same commit that changes
the code. Its section numbering is cited from source comments, so do not renumber.

**[doc/Milestones.md](doc/Milestones.md) is the plan that was carried out** — M0–M9, every box
ticked, each with a note on how the milestone departed from its plan. It is now a record: read
it to find out when and why a mechanism arrived, and do not add work items to it. A milestone's
acceptance criteria are still live constraints, though, and most are checked by the test suite.

**[doc/Release_Notes.md](doc/Release_Notes.md) records why the code is the way it is**, per
milestone. Comments in the source stay terse and say *what*; the *why* goes here — and it is
still the place a substantive change is explained, appended under a new heading rather than by
rewriting a milestone's section. Read the M0 section before touching the allocator, the
coroutine shim, or the build flags — each departs from an obvious approach for a reason stated
there and nowhere else.

## What this project is

Braam is a CLI-oriented operating system that runs entirely in a browser tab: a kernel, shell,
filesystem, terminal, and programs, written in freestanding C++20 and compiled to WebAssembly,
deployable as a static site with no server and no special HTTP headers.

## Build

CMake with a toolchain file, generating Unix Makefiles — so the whole toolchain is clang, cmake,
make and node, with no ninja. The top-level `Makefile` wraps it and configures on first use:

```
make            # build kernel.wasm, the /bin binaries and tests.wasm
make run        # ctest
make serve      # serve build/web/ and open a browser
make release    # pack build/web/ as build/braam-<version>.zip
make clean      # rm -rf build
```

Overrides: `JOBS=1` for a serial build (the default is the CPU count), `GENERATOR=Ninja` if it
is installed and you want the ~30% faster build, `BUILD=<dir>` for the build tree.

`build/web/` is assembled by its own always-run `web` target, not by the kernel's `POST_BUILD`,
so editing a file under `web/` and running `make` refreshes what `make serve` serves. It is
assembled by `copy_directory`, which never *deletes*: a file removed from `web/` lingers in an
old build tree, and `make release` would pack it. Cut a release from a clean tree.

The version reads `0.2.24-35f6924`: `BRAAM_VERSION_BASE` from `src/kernel/version.h` — edited by
hand, `0.2` — then `git rev-list HEAD --count` and `git log -1 --format=%h`. `tools/version.py`
is the one implementation: the always-run `revision` target has it write
`build/gen/kernel/revision.h`, which `version.h` includes, and it rewrites that header only when
the revision moves, so an ordinary build relinks nothing. Taking it at build time is the point —
a commit does not re-run cmake, so a configure-time version would name a release after whatever
the tree was when it was configured. Outside a repository (an unpacked release) the revision is
`0` with no hash. CI checks out with `fetch-depth: 0`, since a shallow clone would call every
revision 1.

`make release` runs `tools/release.py` over `build/web/`, naming the archive after that same
version — the script imports `version.py` rather than restating it, and both run at build time
rather than at configure time. The archive is deterministic: sorted entries and one fixed
timestamp, so the same tree gives the same bytes.

The warning set is `-Wall -Wextra -Wshadow`, and `BRAAM_WERROR` is **ON by default** — so a
warning is a build failure locally and in CI alike, and the tree is warning-clean. Fix the
warning rather than reaching for `-DBRAAM_WERROR=OFF`, which exists for bisecting old commits.

`CMAKE_ARGS` passes flags to the configure step, which only happens on a fresh tree or after
`make clean` — so `make CMAKE_ARGS="-DBRAAM_WERROR=OFF"` on an already-configured tree does
nothing. `-DBRAAM_LLVM=<path>` relocates the toolchain (`-DBRAAM_WASI_SDK=` is the former name,
kept as an alias); CI passes only that, since the warning settings are the defaults. The build
produces `build/kernel.wasm`, a separate `build/test/tests.wasm`, and a ready-to-serve
`build/web/`.

Note that `make -jN` does **not** reach the compiler: make's jobserver descriptors do not
survive the intervening cmake process, so the wrapper passes `-j $(JOBS)` explicitly. Change
`JOBS`, not `-j`.

`BRAAM_LLVM` is used as a clang distribution only. **Nothing from its runtime or its headers is
linked or included** — `-nostdinc++` is not optional, and `--no-default-config` keeps a config
file (wasi-sdk ships one) from injecting a sysroot. libc++'s `<coroutine>` cannot be used
freestanding either way (it pulls in `<cstring>`/`<cmath>`, which need a sysroot the bare
`wasm32-unknown-unknown` target does not have). A hand-written shim over the `__builtin_coro_*`
intrinsics replaces it, at [src/kernel/coroutine.h](src/kernel/coroutine.h).

Any LLVM with the wasm32 target therefore works, and two are supported. Locally, **Homebrew**:
`brew install llvm lld` — `lld` is a separate formula and the `llvm` keg ships no linker at all,
so `wasm-ld` comes from it, and the toolchain file fails at configure time saying so if it is
missing. In **CI**, wasi-sdk 33, unpacked to `/opt/wasi-sdk-33.0` and passed explicitly, because
a pinned tarball cannot drift under a `brew upgrade` the way a rolling keg can. With no
`-DBRAAM_LLVM`, the toolchain file probes `/usr/local/opt/llvm`, `/opt/homebrew/opt/llvm`, then
`/opt/wasi-sdk-33.0`, so a Homebrew machine needs no flag. Both produce a passing build within
~100 bytes of each other. Note that neither provides compiler-rt for `wasm32-unknown-unknown`;
that is invisible only because `-nostdlib` links no builtins, so a construct that needs one
(128-bit division, an outlined `memcpy`) fails on both.

Two link flags from the original Appendix C line are deliberately **absent**, and adding either
back would be a regression: `--export-dynamic` (unreliable — exports are named individually
with `BRAAM_EXPORT`) and `--allow-undefined` (without it, an accidental libc dependency is a
link error rather than a runtime trap). See Concept.md §C.3.

Verification is three CTest cases, run on every build: `smoke` asserts the exact import/export
surface of `kernel.wasm` and of every binary, and that the kernel boots; `unit` runs
`tests.wasm` under Node; and `size` checks `tools/size_budget.txt`. New core code gets a case
in [test/unit/](test/unit/). Behind those, the acceptance criteria in Milestones.md are the
standing behavioural contract — a change that breaks one is a regression however green the
three cases are, so re-check the criteria a change touches by hand at the prompt.

Both wasm modules import the storage and service ABIs, so both are driven with the in-memory
backends in [test/fakefs.mjs](test/fakefs.mjs) and [test/fakesvc.mjs](test/fakesvc.mjs). They
answer from inside the import, which no browser can do and the kernel cannot tell — `wake()`
only queues a resumption. They take their constants and their encoders from `web/fs.js`,
`web/svc.js` and `web/abi.js`, so the two sides of the wire cannot drift silently; keep it that
way rather than restating the format.

A fake proves the kernel, not the shipping JS. `tools/wsd.mjs` is the counterweight for the one
service where that gap matters: a real WebSocket server, so `make serve` gives two tabs a real
conversation rather than a loopback.

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
   ABI. Storage and host services are each multiplexed — one import per calling convention, not
   one per operation — so adding an operation is an enum value on each side, not a new import.
   `ref(slot, obj)` is an export, not an exception: it stores a JS object in the externref table
   and schedules nothing, and so are M8's `sys`/`sys_async`, which are a *process's* imports
   arriving with the pid the host bound to them. Spawning, stepping and killing a process are
   three more `host_svc` operations, not a fourth import.
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
  JS and comes back later — every `HostCall`, storage or service — must be a heap record the
  kernel keeps alive past a cancelled await, never a buffer in the awaiting frame. `wake()` on an
  unclaimed token is what reaps one; that is why `sched_wake` returns a bool. A slot the host
  will deposit into belongs to the record for the same reason: `reserve_ref()`, not a `JsRef` in
  the frame.
- **The externref table is the kernel's; JS never indexes it.** `import_module`/`import_name` do
  not apply to tables, so the table is module-defined: the host deposits through the `ref` export
  and receives an object as an argument of `host_svc`. Do not try to hand the table to JS.
- **A process binary shares headers with the kernel, not code.** `src/proc/` links `alloc.cpp`,
  `result.cpp`, `text.cpp` and `fs/path.cpp` and nothing else from the kernel's trees, plus
  `braam_ui` — because anything reaching a host import would appear in the binary's import list,
  which `test/run.mjs` asserts for every binary. It asserts a *subset*: `true` never makes an
  asynchronous syscall, so it does not import `sys_async` at all, and what matters is that
  nothing else is imported.
  That is why `panic` is declared in `host.h` and defined once per binary, and why it takes
  `(ptr, len)` rather than a `Str`: the wasm ABI passes an 8-byte struct indirectly, and that
  cost 2,812 bytes across the kernel's call sites.
- **Never `new` anything.** `operator new` returns null on failure and `-fno-exceptions` means
  the expression would construct at address zero. Use `heap_new`/`heap_delete` from `alloc.h`.
- **One receiver per `Channel`, and the keyboard's is the tty pump.** A full-screen program
  claims a route through it (`KeyInput`, `InputClaim` in `src/user/tty.h`) rather than receiving
  on `keys()`, which would displace the pump silently and lose `^C`. `^C` is never routed to a
  claimant. And since `CancelState::waiting` is one slot, no task can be parked on a pipe and on
  the keyboard at once — which is why `less` reads its input to the end before it paints.
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

Every program is a binary; there is no in-kernel program and no way to write one. `exec` picks
between the two isolated tiers from binary metadata, so userland does not notice (Concept.md §4):

- **Shell builtin** — not a program and not a file: `cd`, `fg`, `jobs`, `kill`, `help`, `exit`,
  in `src/user/builtin/`. Each is one no syscall could serve — the working directory is one
  global, and the job table and the tty pump are the shell's. A builtin is an ordinary pipeline
  stage, so it pipes, redirects and takes `^C` with nothing added for it. The table is an
  explicit array, not a registrar: `braam_user` is an archive, and `--gc-sections` would drop an
  unreferenced registrar silently — which is the trap `src/prog/` needed an OBJECT library to
  avoid.
- **Separate instance, shared worker** (M8) — address-space, capability, descriptor and
  memory-cap isolation. A binary in `/bin` carrying a `braam` custom section; `exec` reads
  the tier out of it.
- **Separate instance, own worker** (M9) — adds a real kill switch, since wasm cannot be
  preempted: `worker.terminate()`. A binary asks for it with `--tier 3` in `src/cmd/CMakeLists.txt`,
  and runs at tier 2 where the host cannot make a worker. The protocol is one message each way
  per step, the tier rides in the spawn request's `flags` word (`proc_pack` in `sysabi.h`), and
  a tier-3 syscall costs two `postMessage` hops rather than a call — which is why the tier is a
  claim a binary makes rather than a default.

The kernel↔process ABI is Concept.md §4.3 and `src/kernel/sysabi.h`, and both ends include the
header so neither can drift alone. Three rules about it are load bearing:

- **The kernel never calls a process, and the host never calls one while the kernel is on the
  stack.** Only JS can call another instance's exports, and a process calls straight back in
  through `sys`, so stepping one from inside a kernel import would run kernel code on a
  half-changed heap. A step is queued and drained after `tick()` returns — a microtask in
  `web/worker.js`, an explicit `drain()` in the test driver. Synchronous syscalls are the other
  direction and re-enter the kernel at top level, exactly as `key()` does.
- **A process's pid is written into its import closure, not passed.** That is the whole of "a
  process cannot issue a syscall on behalf of another PID": there is no argument for it. At
  tier 3 the pid is bound into the worker at creation, and the step protocol's messages carry
  *that* pid — never one read out of a message body, which would give it back.
- **A process may have several syscalls outstanding, and the step says which one it answers.**
  `PROC_TASKS` is 4 on the process side; on the kernel side each parked call is a `Call` record
  with its own staging block, served by a scheduler job of its own. One reused staging buffer
  would let the second call overwrite the first, and one proxy performing them in turn would let
  a socket read that never completes starve the keystroke behind it. The resume token rides in
  the step request's `flags`.
- **A process ends when its root task returns**, whatever the others are doing — as a process
  ends when main does. The kernel then drops the instance and cancels the servers of anything
  the other tasks had outstanding.
- **The in-wasm unit tests cannot run a program at all.** Stepping one means returning to the
  host, and `run_tests()` does that once. With no applets left, `test/unit/` can drive only the
  six builtins — which is enough for pipelines (`jobs | help` is two real stages), redirection,
  the boot-cost guard and the leak check. Everything that needs a program to actually run is in
  `test/run.mjs`: put it there rather than reaching for a test-only applet.
- **Both halves of the tier-3 step protocol live in `web/proc.js`** — `serveProc` is the
  process's side and `makeProc` the host's, and `web/procworker.js` and `test/fakeworker.mjs`
  are wiring around them. Two files describing one wire is how it drifts.
- **A terminated worker's in-flight step must be failed by whoever killed it.** An abandoned
  `HostReq` is reaped by `wake()` on its token and by nothing else, so a request nobody will ever
  answer leaks the record and its payload for the life of the page.

A tier-2 program is an ordinary scheduler job: a proxy task in `src/user/exec.cpp` steps the
instance and performs its syscalls with its own `CancelToken`, so `^C`, `kill`, `jobs`, `/proc`
and the stage epilogue need nothing added. Its destructor drops the instance — or, at tier 3,
terminates the worker holding it, which is the same sentence one thread further out.

Since M4 a pipeline's stages are independent scheduler jobs rather than a child group the shell
`co_await`s: `CancelState::waiting` is a single slot, so one job cannot have two children parked
at once. §3.6's structured concurrency is put back by hand, from a destructor in `run_line`'s
frame. `chat` is the second case, and it shows the cost: a cancelled child does not unwind until
the scheduler resumes it, a tick or two after its parent is gone, so it must touch nothing the
parent owns — its session is refcounted and it writes to the screen rather than through a
`Stream` whose pipe belongs to a `Job` that may already be freed. A real child-group awaitable
needs intrusive queue links inside `Waiter` first — the same work a channel with two blocked
senders would need, which `Channel::park_sender` panics on today rather than losing a wakeup
quietly.

## Known gaps

These are absent on purpose, each for a reason recorded in Milestones.md or Release_Notes.md.
None is a bug, and adding one is a design change to be argued in Concept.md first:

- **No `bg` and no `^Z`.** Stopping a running coroutine at an arbitrary point is the
  resume-side twin of `CancelToken` and would have to reach every awaitable.
- **Resize drops rows from the top rather than re-wrapping logical lines**, which §3.5 had
  promised to M7.
- **One global working directory.** A process is isolated in address space, memory and
  descriptors, but not in the namespace it can name. It is the reason `cd` is a builtin and
  `pwd` reads `/proc/cwd` rather than either being a syscall.
- **No CPU metering.** Tier 3 kills a runaway program; nothing bounds one. Fuel injection was
  considered and not built.
- **`Pane` is a primitive, not a multiplexer.** Two jobs visible at once needs per-pane output
  routing and a window manager in the shell. One process at a time may hold the screen: a second
  `ScreenEnter` is refused with `Err(Perm)` rather than left to nest politely.
- **Every command costs an instantiation** — roughly a millisecond, plus reading the image out of
  `BundleFs`, where an applet cost nothing. The host caches the compiled `Module` by path, so the
  bytes still cross the VFS on every `exec` and only the compile is saved.
- **The boot archive is ~379 KB**, against 47 KB when four programs were binaries. That is §4.4's
  duplication: every binary carries the allocator, the string types and the coroutine runtime.
  `bundle.bin` carries a size budget of its own, so the number stays visible.
- **Two tier-3 fidelity losses (§4.3):** a binary that will not instantiate reads as a crash
  rather than as a refusal, and `Sys::Now` is relative.

## Conventions

- **Keep comments in code and scripts terse.** A comment says what a thing is, not why the
  design is the way it is. Reasoning, alternatives and trade-offs belong in
  [doc/Release_Notes.md](doc/Release_Notes.md) or another document, where they can be read as
  prose and revised in one place.
- Commits: no `Co-Authored-By` trailer, no generated-with footer. Commit only when asked.
- Layout: `src/kernel/`, `src/fs/` (paths, the VFS, the filesystems, the host storage ABI),
  `src/svc/` (fetch, WebSocket, clipboard, file transfer, wall clock, the process operations),
  `src/ui/` (the layout layer over a `Grid`: `Pane`, `TextBuf`, `TextView`), `src/user/` (line
  editor, grammar, job runtime and job table, shell, `exec`, `ProcFs`, boot) with
  `src/user/builtin/` under it, `src/proc/` (a process binary's runtime, `screen.cpp` included)
  and `src/cmd/` (one file per program), `test/unit/`, `web/` (`proc.js` both halves of the
  process protocol, `procworker.js` a tier-3 process's worker), `bundle/`, `tools/`, `cmake/`.
  Concept.md §7. `braam_fs` and `braam_svc` are siblings above the kernel and below userland and
  must not depend upwards or on each other; anything needing the job table or the shell belongs
  in `src/user/`, which is why `ProcFs` lives there. **`braam_ui` is no longer one of them**: it
  links `braam_flags` alone and the kernel does not link it at all, because `less` and `edit` are
  binaries and a process links it instead. Keep it clear of the VFS, the screen and every host
  import, or the exact-import assertion over each binary will say so.
- **The builtin table is an explicit array, and must stay one.** `braam_user` is `STATIC`, and
  `--gc-sections` never extracts an unreferenced archive member, so a self-registering builtin
  would be dropped without a word. This is why `src/prog/` had to be an `OBJECT` library while it
  existed; six entries named in one file need no such trick and no count tripwire.
- Exports are declared with `BRAAM_EXPORT("name")`, imports with `BRAAM_IMPORT("name")` — never
  by linker flag. Either one changes the ABI, so update the expected surface in
  [test/run.mjs](test/run.mjs) in the same commit.
- `.clang-format` at the root is authoritative: 4-space indent, 100 columns. Types are
  `PascalCase`, functions and variables `snake_case`, constants `SCREAMING_SNAKE`.

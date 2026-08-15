# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository state

**The plan is finished: M0–M9 are all done.** Since then the kernel applet has been retired and
then the shell followed it: every program is a binary, including `/bin/sh`, and thirty-two of
them live in `src/cmd/` with the shell's own parts in `src/sh/`. There is no in-kernel program of
any kind and no program registry. `kernel.wasm` is about 137 KB against a 256 KiB budget and
`bundle.bin` is 491 KB; the wasm ABI is still six imports and nine exports, and the three CTest
cases pass. Work here is change to a working system, so the bar is that nothing above regresses,
and Milestones.md is history rather than a to-do list.

What each milestone left behind. The layout still reflects it except where the shell is
concerned — `src/user/parse.cpp`, `edit.cpp`, `job.cpp` and `builtin/` below are all in `src/sh/`
now, and `shell.cpp` is `src/cmd/sh.cpp`:

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
favour of `/bin` and `/share` as two views of the one bundle, the §4.3 syscall table grown from
eight operations to twenty-seven to meet what the applets used to reach for directly, `src/ui/`
turned into a library over
a `Grid` that a process links, and the step protocol given a token so a process can have several
calls outstanding at once.

**Since then, a process can start a process.** The §4.3 table gained `chdir` beside the
filesystem operations and `pipe`/`spawn`/`wait`/`kill` as a family at 80. Every process has a
working directory of its own, inherited from its spawner. A descriptor named in a spawn is
*moved* into the child, and a child is an ordinary scheduler job its parent's destructor cancels.
`timeout` and `watch` were the first callers.

**And then the shell became a program.** `/bin/sh` is a binary init runs; `src/user/shell.cpp`,
`edit.cpp`, `job.cpp` and `src/user/builtin/` are gone, and the same code lives in `src/sh/` over
the syscall table. Two operations were added for it — `cursor` at 69, the scrolling screen's
cursor, and `fg` at 84, which names what `^C` reaches. The tty pump is now permanent and init
spawns it (`src/user/console.h`), because something has to hold the keyboard while nothing is
running and a process has no `keys()`. None of it touched the wasm ABI, and the only JavaScript
that changed was splitting `proc.shutdown()` so it stops killing tier-2 processes.

**And the prompt got a colour.** `style` at 70 sets what the next `Write` paints with — sticky
grid state, the colour a cell grid cannot carry in the bytes (§2.3) — so the table is thirty-five
and `PROC_ABI` is 5. `/bin/sh` is the only caller: red `[N]`, bright white `$`, and the default
back before what is typed.

**And the clipboard goes both ways.** `Cmd+V` — `Ctrl+V` where that is the chord — types the
clipboard into the terminal: `pasted()` in `web/keys.js` turns the text into the run of
keystrokes that would have typed it, and `web/worker.js` feeds it at the rate the console drains
it. That pacing is why `key()` now returns 1 or 0 rather than nothing — the ring holds 64 and a
paste is longer — which is the one signature change on the boundary; the export list, the import
list and the §4.3 table are all untouched. A waiting `pbpaste` still takes the gesture instead.

**And the prompt says where you are.** `home $` — the working directory's basename, plain white
on blue, then a black space and the bright white `$`. Nothing was added for it: `Prompt` in
`src/sh/edit.h` grew a third field, `anchor` a third styled run, and `interactive()` calls
`cwd_get()` once per line rather than caching what `cd` returns, because a stale prompt is
believed. The basename comes from `path_basename`, which already answers `/` at the root, and it
points into a `String` the loop body owns — `Prompt`'s `Str`s are non-owning and now one of them
is not a literal. `test/run.mjs` composes the expected prompt from the directory it is in rather
than spelling it.

**And every program took a worker.** doc/TODO.md T3: tier 3 is now the default
`cmake/BraamProgram.cmake` gives a program, and `/bin/sh` is the one binary that asks for tier 2
— so thirty-one of the thirty-two run in a worker of their own and can be killed rather than
waited on. Nothing in C++ moved: the tier is a `u32` in the `braam` custom section and the same
binary runs at either (§4.3), so this is `stamp.py`'s argument, `test/run.mjs`'s `want_tier`, and
the documents the change made false. What did move is the test driver's model of a worker:
`net.hold(n)` in `test/fakeworker.mjs` counts binds now, because `clear` is a program too and a
spawning program binds its own worker before its child's, and the two cases that give the tier up
run last so that everything before them runs at the tier it ships at.

**And the shell got a pid.** It answered to 0, which is what `sched_spawn` returns on failure and
what the terminal claims mean by "nobody" — so `Sys::Fg`'s "nobody holds the keys" clause passed for
a coincidence of sentinels rather than for a reason, and T8 was about to put a worker behind that
pid. It takes init's pid now, and the rule gained the clause it meant: the foreground belongs to
whoever armed it. `cat | wc` with a `^C` is the case that would have caught the difference, and
nothing like it existed.

**And init started replacing the shell.** doc/TODO.md T7, the design question T8 waits on: a
tier-3 process dies with its worker and there is no falling a running one back, so once `/bin/sh`
is one of them a lost worker would be the session rather than a command. `init_task` is a loop
now — a shell that **died** is replaced by an ordinary `exec`, which lands at whatever tier is
left; one that **exited** is not, so `exit` still ends the session. `exec_process` grew a
`bool *died` because an `i32` cannot tell `exit 132` from a trap. It also found a bug that would
have decided T7 by itself: `dropWorkers()` terminated a worker without failing the step in it,
which is a kernel parked for ever rather than a process that died.

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

**[doc/System_Calls.md](doc/System_Calls.md) explains the kernel↔process mechanism end to end**
— the deferred step, the staging protocol, the two token namespaces, cancellation and the kill,
with sequence diagrams and the whole operation table in one place. It is *derived*, not
normative: Concept.md §4.3 is still the specification, so read System_Calls.md to understand the
mechanism and amend §4.3 to change it. Anything touching `src/proc/`, `src/user/exec.cpp`,
`src/kernel/sysabi.h` or `web/proc.js` has to keep it true.

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
make bench      # what a syscall costs at each tier, in a browser (doc/TODO.md T1)
make install    # the SDK, to /usr/local if writable else ~/.local
make release    # pack build/web/ and the SDK as build/*.zip
make clean      # rm -rf build
```

Overrides: `JOBS=1` for a serial build (the default is the CPU count), `GENERATOR=Ninja` if it
is installed and you want the ~30% faster build, `BUILD=<dir>` for the build tree,
`PREFIX=<dir>` for `install`.

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
timestamp, so the same tree gives the same bytes. It then installs into `build/sdk` and packs
that as `braam-sdk-<version>.zip`, so the SDK archive and `make install` cannot disagree; that
stage is never emptied either.

**The SDK is `make install` and that second archive** — [doc/Programming_Manual.md](doc/Programming_Manual.md) is the
user-facing guide. It is headers (`include/braam/{kernel,fs,proc,ui}/`, whole directories, so
no include closure to maintain), `libbraam_proc.a` and `libbraam_ui.a` and no other library —
the rest carry host imports — the toolchain file, `stamp.py`, and a relocatable CMake package
that finds its own prefix, so an unpacked zip is a working SDK with nothing installed. Three
things the project used to supply implicitly are now on `braam_flags` or the toolchain, because
a consumer had none of them: `cxx_std_20` (a coroutine `Task` will not parse without it),
`CMAKE_BUILD_TYPE MinSizeRel` (at `-O0` a freestanding build calls libcalls nothing provides),
and `-Werror` behind `$<BUILD_INTERFACE:>`.

**The program recipe is `cmake/BraamProgram.cmake`, shared by `src/cmd/` and the SDK.**
`braam_add_program(NAME … SOURCES … [TIER] [LIBS])` — one implementation, so an out-of-tree
program is built by the same code rather than by a description of it. `examples/hello/` is the
worked example the SDK installs *and* an ordinary build target, from one `CMakeLists.txt`, so
it cannot rot; it is not packed into the bundle.

The warning set is `-Wall -Wextra -Wshadow`, and `BRAAM_WERROR` is **ON by default** — so a
warning is a build failure locally and in CI alike, and the tree is warning-clean. Fix the
warning rather than reaching for `-DBRAAM_WERROR=OFF`, which exists for bisecting old commits.

`CMAKE_ARGS` passes flags to the configure step, which only happens on a fresh tree or after
`make clean` — so `make CMAKE_ARGS="-DBRAAM_WERROR=OFF"` on an already-configured tree does
nothing. `-DBRAAM_LLVM=<path>` relocates the toolchain; CI passes nothing at all, since the
warning settings are the defaults and clang is on its PATH. The build produces `build/kernel.wasm`, a separate `build/test/tests.wasm`, and a ready-to-serve
`build/web/`.

Note that `make -jN` does **not** reach the compiler: make's jobserver descriptors do not
survive the intervening cmake process, so the wrapper passes `-j $(JOBS)` explicitly. Change
`JOBS`, not `-j`.

The toolchain is a clang distribution only. **Nothing from its runtime or its headers is linked
or included** — `-nostdinc++` is not optional, and `--no-default-config` keeps a config file
from injecting a sysroot. libc++'s `<coroutine>` cannot be used freestanding either way (it
pulls in `<cstring>`/`<cmath>`, which need a sysroot the bare `wasm32-unknown-unknown` target
does not have). A hand-written shim over the `__builtin_coro_*` intrinsics replaces it, at
[src/kernel/coroutine.h](src/kernel/coroutine.h).

Any clang with the wasm32 target therefore works, and it is plain clang everywhere — there is no
SDK. Locally, **Homebrew**: `brew install llvm lld`, `lld` being a separate formula since the
`llvm` keg ships no linker at all. In **CI**, Debian's `clang lld llvm`, unversioned. The
toolchain file finds each tool with `find_program`, probing `/usr/local/opt/llvm` and
`/opt/homebrew/opt/llvm` first because Homebrew keeps its keg off PATH, and it fails at
configure time naming whichever of clang, clang++, llvm-ar, llvm-ranlib or wasm-ld is missing.
The wasm features are named in the toolchain file rather than taken from the compiler's default
CPU, which is not the same set on clang 18 as on clang 22: `-mreference-types` (without it
`__externref_t` is an unknown type), `-mbulk-memory` (without it `memcpy` is a call to something
nothing defines), and `-msign-ext -mmutable-globals -mnontrapping-fptoint` for parity. The list
is verified sufficient by building with `-mcpu=mvp` on top of it, which is the oldest baseline
there is.

Note that neither distribution provides compiler-rt for `wasm32-unknown-unknown`; that is
invisible only because `-nostdlib` links no builtins, so a construct that needs one (128-bit
division, an outlined `memcpy`) fails on both.

Two link flags from the original Appendix C line are deliberately **absent**, and adding either
back would be a regression: `--export-dynamic` (unreliable — exports are named individually
with `BRAAM_EXPORT`) and `--allow-undefined` (without it, an accidental libc dependency is a
link error rather than a runtime trap). See Concept.md §C.3.

Verification is three CTest cases, run on every build: `smoke` asserts the exact import/export
surface of `kernel.wasm` and of every binary, and that the kernel boots; `unit` runs
`tests.wasm` under Node; and `size` holds `kernel.wasm` and `bundle.bin` to
`tools/size_budget.txt` — a program is measured but not bounded, so there is no per-binary
budget and adding one back is a design change. New core code gets a case
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

**`make bench` is the other counterweight, and it is a measurement rather than a test.** The cmake
`bench` target packs two twins of the boot archive by re-stamping the staged binaries — one tier at
each end, `bundle2.bin` and `bundle3.bin`, with the shipped `bundle.bin` as the middle arm; nothing
is recompiled and `bundle.bin` is byte-identical — `web/bench.html` drives the shipped page against
all three, and `tools/bench.mjs` serves them and collects what the page posts into
`build/bench-<engine>.json`. It answers what a fake cannot: what a syscall costs on a real
`postMessage`. The counters behind it are unconditional and live in `makeProc`'s `stats()` and in
`web/worker.js`; the figures are in doc/TODO.md T1 and T5. **The arm ids mean what they meant at
T1** — `t2` every program at tier 2, `t3nosh` tier 3 but for the shell, `t3` all of it — so the two
measurements can be read against each other; keep it that way, since T5's own code change was
repairing an arm that had silently stopped being a control.

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
   Mouse selection and copy live entirely on the page and in `web/render.js` for the same reason
   — the grid is already shared, so a selection needs no import, no export and no syscall, and
   there is no mouse anywhere in the ABI (Concept.md §3.5).

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
- **One receiver per `Channel`, and the keyboard's is the console pump.** It is permanent and
  init spawns it (`src/user/console.h`); it used to be spawned per foreground pipeline, and could
  not stay that way once the shell became a process, because something has to hold the keyboard
  while nothing is running. A program claims a route through it (`KeyInput` in `src/user/tty.h`)
  rather than receiving on `keys()`, which would displace the pump silently and lose `^C` — and
  the prompt is no exception. Since `CancelState::waiting` is one slot, no task can be parked on
  a pipe and on the keyboard at once, which is why `less` reads its input to the end before it
  paints.

  **`^C` cancels the foreground if there is one and is delivered to the claimant if there is
  not.** The foreground is a set of pids armed with `Sys::Fg`; a shell arms its stages before it
  waits, and at a prompt arms nothing, so the interrupt arrives as an ordinary key and abandons
  the line. Without that split a shell that is a process would be killed by its own `^C`.

  **The foreground belongs to whoever armed it**, which is the fourth clause of `Sys::Fg`'s
  authorisation and the only one that lets a shell arm a *pipeline*: it lets the keyboard go
  before it spawns, so from the second stage on it holds neither the keys nor a place in the set
  it is filling (§4.3). Nothing removes one pid from that set either — the shell clears it after
  collecting, and init clears it between shells, and those are the only two things that do.

  **0 is not a pid.** It is what `sched_spawn` returns on failure, what `tty_keys_owner()` and
  `tty_screen_owner()` mean by "nobody", what `SYS_WAIT_ANY` is, what `Fg(0)` means, and what
  `link.pid = 0` means in `web/proc.js`. `/bin/sh` used to answer to it — init ran it from a
  default-constructed `Executable` — which made `Sys::Fg`'s "nobody holds the keys" clause pass
  for the wrong reason. It takes init's pid now, since it is a process inside init's task rather
  than a job of its own, and `/proc/<init>` shows its cwd for the same reason.

  Each of the two routes has **one holder, and a second claim is `Err(Perm)` rather than a
  nested one**, named by the pid that took it. A claim clears its route only if it is still the
  holder, so a parent and its child may die in either order. Nesting meant restoring a
  predecessor that may already be gone — a freed key ring, or for `FullScreen` a snapshot of the
  blank grid the first claimant was painting.

  The same rule is why a `Sys::Spawn` **moves** a descriptor into the child rather than
  duplicating it: one end, one owner, so two blocked senders cannot be arranged from userland —
  and why one a syscall of the parent is parked on cannot be moved at all. Within a process, a
  second concurrent use of a *descriptor* in the same direction is `Err(Perm)`, for two different
  reasons: on a pipe end `Channel::park_sender` panics and a second receiver is displaced
  silently, and on the host kinds `svc_blob`'s sized-twice reply is not re-entrant per object.
- **A descriptor is held for the length of a syscall.** `Handle` is refcounted, so `Close` frees
  the number and *shuts* what is behind it at once — that is what answers a parked read — while
  the block and its externref slot wait for the last call. The slot must not come back before
  then: `jsref_release` recycles it, and `HostCall::issue()` re-reads it on the second attempt.
- **Every awaiter deregisters in its destructor.** `sched_unwait` from `~Awaiter` is what makes
  destroying a suspended frame safe rather than a dangling `Waiter *` in the wake table. An
  awaitable that parks and has no destructor is a use-after-free waiting to happen.
- **Whoever may still touch a `Stdio` holds a reference to what is behind it.** The three streams
  are function pointers into a block somebody else owns — the shell's `Job`, for a pipeline
  stage — and cancellation is deferred, so a frame parked on one unwinds a tick after the stage
  that built it is gone. `Stdio::hold`/`owner` names that block; `Proc` retains it and releases it
  last in `~Proc`, and a syscall server takes a counted `ProcRef` **by value as a coroutine
  parameter**. That is not style: a parameter copy is destroyed after the body's locals, and one
  of those locals is the awaitable that deregisters from the pipe.
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

- **Shell builtin** — not a program and not a file, and no longer kernel code either: `cd`, `fg`,
  `jobs`, `kill`, `help`, `exit` live in `src/sh/builtin/`, inside `/bin/sh`. What makes one a
  builtin is that it touches the shell *process's* own state — its working directory, which a
  typed command inherits at spawn; its job table, which no syscall shows to anyone; its loop. A
  builtin pipes and redirects by reading and writing descriptors like anything else, but it runs
  **in its turn rather than alongside**, because nothing inside a process can wait for a sibling
  task — so a builtin must buffer its output and write it once, or it fills an eight-slot pipe
  and parks with nobody left to drain it. The table is an explicit array, not a registrar:
  `braam_sh` is an archive, and `--gc-sections` would drop an unreferenced registrar silently.
- **Separate instance, shared worker** (M8) — address-space, capability, descriptor and
  memory-cap isolation. A binary in `/bin` carrying a `braam` custom section; `exec` reads
  the tier out of it. `/bin/sh` is the only binary that asks for it: `set(BRAAM_BIN_TIER_sh 2)`
  in `src/cmd/CMakeLists.txt`, because a prompt pays the tier several times a keystroke.
- **Separate instance, own worker** (M9) — adds a real kill switch, since wasm cannot be
  preempted: `worker.terminate()`. **This is the default** — `braam_add_program` stamps tier 3
  unless a program asks for 2, so every program but the shell has one — and it runs at tier 2
  where the host cannot make a worker. The protocol is one message each way per step, the tier
  rides in the spawn request's `flags` word (`proc_pack` in `sysabi.h`), and a tier-3 syscall
  costs two `postMessage` hops rather than a call: 34–45 µs measured, which doc/TODO.md T1 is
  the argument for paying everywhere and T5 re-measured unmoved after the flip.

**A process that loses its worker dies with it, and init replaces the shell** (doc/TODO.md T7).
§4's tier-2 fallback is decided before a process starts, so it covers a host that never had
workers and not one whose workers go mid-session — there is no falling a *running* process back,
since the instance went with the worker. So init starts another `/bin/sh` when its shell **died**
(a trap, a step that failed, an instance that would not be made) and does not when it **exited**:
`exit` still ends the session. `exec_process`'s `bool *died` is what says which, since `exit 132`
and a trap are the same `i32`. Bounded at three deaths in quick succession. Whoever takes a worker
away — `kill()` or `dropWorkers()` — must fail the in-flight step, or the kernel is parked for
ever on a reply that is not coming.

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
- **A process's children are cancelled by its destructor**, the way `run_line` cancels a
  pipeline's stages: §3.6's structured concurrency, put back by hand one level further down. A
  child is an ordinary scheduler job, so `^C`, `kill`, `jobs` and `/proc` reach it with nothing
  written for them, and its status is recorded on the parent's record by a destructor that finds
  the parent by pid — pids are never reused, so that lookup cannot land on a different process.
- **A process ends when its root task returns**, whatever the others are doing — as a process
  ends when main does. The kernel then drops the instance and cancels the servers of anything
  the other tasks had outstanding.
- **The in-wasm unit tests cannot run a program at all,** and the shell is one. Stepping an
  instance means returning to the host, and `run_tests()` does that once. What `test/unit/` can
  still reach is everything *below* a program: the console and its claims (`test_console.cpp`,
  `test_tty.cpp`), the pipes, `/proc`, the VFS, and the grammar — `parse.cpp` and `tokenize.cpp`
  are pure and are compiled straight into the suite rather than linked from `braam_sh`, which
  would drag the process runtime's imports in with them. Anything that needs the shell or a
  program to actually run belongs in `test/run.mjs`.
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
- **A multi-line paste loses everything after the first command.** That is type-ahead across a
  command boundary, not the paste: the shell gives the raw route back around anything it runs, so
  what is typed while a command runs is cooked into the console channel as *its* stdin, which the
  shell's editor never reads and `console_fg_set` clears. Pasting one line is exact.
- **A mouse selection does not survive output.** It is dropped by the next keystroke and by a
  resize, and output scrolling under it leaves the highlight on cells that have since changed.
  Holding one needs the per-row continuation bit the re-wrap above is waiting for.
- **No per-process root.** A process has its own working directory now, but once a path is
  absolute `open` resolves it with the kernel's full authority. A per-process mount view is a
  milestone's worth of work in the VFS. `cd` is still a builtin because what it moves is the
  *shell's* cwd, which is what a typed command inherits; `/proc/cwd` is that one, and a process's
  own is a line in its `/proc/<pid>`.
- **Ordinary output is not held to the screen claim.** `ScreenBlit` is refused from a process
  that does not hold the screen, but a background job still writes to the grid through `stdout`
  and `ScreenClear` is open to anyone — `clear` and `watch` call it without claiming. Gating that
  is per-job output routing, not a claim.
- **No CPU metering.** Tier 3 kills a runaway program; nothing bounds one. Fuel injection was
  considered and not built.
- **`Pane` is a primitive, not a multiplexer.** Two jobs visible at once needs per-pane output
  routing and a window manager in the shell. One process at a time holds the screen — a second
  `ScreenEnter` is `Err(Perm)` rather than nesting politely — and giving it back to a parent when
  a child is done is the window manager's problem, not the claim's.
- **Every command costs an instantiation and a worker** — roughly a millisecond, plus reading
  the image out of `BundleFs`, where an applet cost nothing. The host caches the compiled
  `Module` by path, so the bytes still cross the VFS on every `exec` and only the compile is
  saved; the worker comes from the pool, and only a pipeline wider than `MAX_IDLE` or a killed
  process makes the host start one.
- **Every syscall a program makes is two `postMessage` hops**, 34–45 µs, since T3 put every
  program in its own worker. `sh` is the exception and stays at tier 2 for it. Bulk I/O pays it
  per `SYS_CHUNK`, which is 512 bytes: doc/TODO.md T5 re-measured that at 6–13 ms more than tier 2
  for a quarter of a megabyte through three processes, and **decided against T6** — a bigger chunk
  or a batched step protocol — because nothing written for this system can perceive it. A workload
  that moves megabytes is what would reopen it, not a better figure.
- **The boot archive is ~491 KB**, against 47 KB when four programs were binaries. That is §4.4's
  duplication: every binary carries the allocator, the string types and the coroutine runtime,
  and `sh.wasm` is 86 KB of it. `bundle.bin` carries a size budget and the binaries under it do
  not, so that one number is where the duplication stays visible.
- **`kill <pid>` is gone; `kill %n` is not.** `Sys::Kill` refuses anything that is not a child of
  the caller, and a bare pid the shell never started is exactly that.
- **No `/proc/jobs`.** The job table is the shell process's own memory, and no syscall shows one
  process another's. The stages are still tasks, so `/proc/<pid>` lists them — which is how the
  shell notices a background job has finished.
- **A replaced shell is a fresh one, not the one that died.** Init respawning `/bin/sh` (T7) keeps
  the session, not the state: the new shell starts in the kernel's `/home` with an empty job table,
  and whatever the dead one had backgrounded went with it, since a process's children are cancelled
  by its destructor. Carrying any of it over means a shell that can be handed its predecessor's
  descriptors, which nothing in §4.3 offers.
- **A keystroke at the prompt costs about six ticks**: `key_read`, then a repaint of four
  syscalls, each a park and a step. It was one channel receive when the editor was kernel code.
  Nothing about it is wrong; it is what §4.4's cost model looks like on the interactive path.
- **The shell has no variables, no `-c`, no globbing and no scripts beyond `sh -s`.** None of it
  was blocked by the shell being kernel code, and none of it is blocked now.
- **Two tier-3 fidelity losses (§4.3),** true of every program since T3 rather than of two: a
  binary that will not instantiate reads as a crash rather than as a refusal, and `Sys::Now` is
  relative. Nothing calls `proc_now()`, so the second is a constraint on what may be written
  next rather than a regression.

## Conventions

- **Keep comments in code and scripts terse.** A comment says what a thing is, not why the
  design is the way it is. Reasoning, alternatives and trade-offs belong in
  [doc/Release_Notes.md](doc/Release_Notes.md) or another document, where they can be read as
  prose and revised in one place.
- Commits: no `Co-Authored-By` trailer, no generated-with footer. Commit only when asked.
- Layout: `src/kernel/`, `src/fs/` (paths, the VFS, the filesystems, the host storage ABI),
  `src/svc/` (fetch, WebSocket, clipboard, file transfer, wall clock, the process operations),
  `src/ui/` (the layout layer over a `Grid`: `Pane`, `TextBuf`, `TextView`), `src/user/` (`exec`
  and the syscall dispatcher, the console and its pump, the pipes behind a stage's stdio,
  `ProcFs`, boot and init), `src/proc/` (a process binary's runtime, `screen.cpp` included),
  `src/sh/` (the shell: grammar, line editor, job runtime, builtins) and `src/cmd/` (one file per
  program, `sh.cpp` among them), `test/unit/`, `web/` (`proc.js` both halves of the process
  protocol, `procworker.js` a tier-3 process's worker), `bundle/`, `examples/` (a program built
  against the installed SDK), `tools/`, `cmake/` (the toolchain file, `BraamProgram.cmake` and
  the package config, all three installed). Concept.md §7. `braam_fs` and `braam_svc` are siblings above the kernel and below userland and
  must not depend upwards or on each other; anything needing the scheduler or the screen belongs
  in `src/user/`, which is why `ProcFs` lives there. **`braam_sh` links `braam_proc`**, so
  nothing in it may reach a kernel header that pulls in the scheduler — the exact-import
  assertion over `sh.wasm` will say so. **`braam_ui` is no longer one of them**: it
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

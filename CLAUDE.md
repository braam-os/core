# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository.

## What this project is

Braam is a CLI-oriented operating system that runs entirely in a browser tab: a
kernel, shell, filesystem, terminal, and programs, written in freestanding C++20
and compiled to WebAssembly, deployable as a static site with no server and no
special HTTP headers.

It is a working system; the bar for any change is that nothing below regresses.
`kernel.wasm` is ~149 KiB against a 256 KiB budget, the boot archive's staging
tree ~710 KiB against 1 MiB, the wasm ABI is six imports and nine exports, and
the three CTest cases pass.

## Documents

- **[doc/Concept.md](doc/Concept.md) is the specification.** Read it before
  doing anything substantive — it carries decisions whose rationale is not
  recoverable from the code. Amend it only when a design decision changes, in
  the same commit as the code. Its section numbering is cited from source
  comments: do not renumber.
- **[doc/Release_Notes.md](doc/Release_Notes.md) records why the code is the way
  it is.** Source comments say *what*; the *why* goes here, appended under a new
  heading rather than by rewriting an existing section. It also holds M0–M9's
  objectives and the twenty-two acceptance criteria — a record, not a to-do
  list, but the criteria are live constraints. Read its M0 section before
  touching the allocator, the coroutine shim, or the build flags.
- **[doc/System_Calls.md](doc/System_Calls.md)** explains the kernel↔process
  mechanism end to end: the deferred step, staging, the two token namespaces,
  cancellation, the kill, and the whole operation table. It is *derived* — §4.3
  is normative — and anything touching `src/proc/`, `src/user/`,
  `src/kernel/sysabi.h` or `web/proc.js` must keep it true.
- **[doc/Programming_Manual.md](doc/Programming_Manual.md)** is the SDK's
  user-facing guide.
- **[doc/Package_Management.md](doc/Package_Management.md)** is the policy
  `/bin/pkg` must satisfy, written before the code: what a package has to prove,
  and how the signing keys are held. It is *derived* from nothing — it states
  policy Concept.md does not carry — so a change to the trust model, the roles,
  the algorithms or the verification order belongs there first. `/bin/pkg` does
  not exist yet.

## Build

CMake with a toolchain file, generating Unix Makefiles: the toolchain is clang,
cmake, make and node, no ninja. The top-level `Makefile` wraps it and configures
on first use:

```
make            # build kernel.wasm, the /bin binaries and tests.wasm
make run        # ctest
make serve      # serve build/web/ and open a browser
make install    # the SDK, to /usr/local if writable else ~/.local
make release    # pack build/web/ and the SDK as build/*.zip
make clean      # rm -rf build
```

Overrides: `JOBS=1` for a serial build (default is the CPU count),
`GENERATOR=Ninja` (~30% faster), `BUILD=<dir>`, `PREFIX=<dir>`. `make -jN` does
**not** reach the compiler — make's jobserver descriptors do not survive the
intervening cmake process — so change `JOBS`, not `-j`. `CMAKE_ARGS` reaches
only the configure step, which happens on a fresh tree or after `make clean`.

`build/web/` is assembled by its own always-run `web` target via
`copy_directory`, which never deletes: a file removed from `web/` lingers in an
old build tree. Cut a release from a clean tree.

The version reads `0.2.24-35f6924`: `BRAAM_VERSION_BASE` from
`src/kernel/version.h` (hand-edited) plus `git rev-list HEAD --count` and
`git log -1 --format=%h`. `tools/version.py` is the one implementation and runs
at *build* time — a commit does not re-run cmake — rewriting
`build/gen/kernel/revision.h` only when the revision moves. Outside a repository
the revision is `0`; CI checks out with `fetch-depth: 0`. `make release` and
`tools/release.py` import it rather than restating it, and pack with sorted
entries and one shared stamp — the pack time, or `SOURCE_DATE_EPOCH` (read as
UTC) when it is set, which is what makes a pack reproducible.

The SDK is `make install` plus that second archive: headers
(`include/braam/{kernel,fs,proc,ui}/`, whole directories), `libbraam_proc.a` and
`libbraam_ui.a` and no other library (the rest carry host imports), the
toolchain file, `stamp.py`, and a relocatable CMake package that finds its own
prefix. `braam_flags`/the toolchain must keep supplying `cxx_std_20`,
`MinSizeRel` (at `-O0` a freestanding build calls libcalls nothing provides) and
`-Werror` behind `$<BUILD_INTERFACE:>`.

The program recipe is `cmake/BraamProgram.cmake` —
`braam_add_program(NAME … SOURCES … [LIBS])` — shared by `src/cmd/` and the SDK,
so an out-of-tree program is built by the same code. `examples/hello/` is both
the SDK's worked example and an ordinary build target, so it cannot rot.

Warnings are `-Wall -Wextra -Wshadow` with `BRAAM_WERROR` **ON by default**; the
tree is warning-clean. Fix the warning rather than reaching for
`-DBRAAM_WERROR=OFF`, which exists for bisecting old commits.

### Toolchain

A clang distribution only, and **nothing from its runtime or headers is linked
or included**: `-nostdinc++` is not optional and `--no-default-config` keeps a
config file from injecting a sysroot. libc++'s `<coroutine>` is unusable
freestanding (it pulls in `<cstring>`/`<cmath>`); the shim over
`__builtin_coro_*` at [src/kernel/coroutine.h](src/kernel/coroutine.h) replaces
it.

Any clang with the wasm32 target works — Homebrew `llvm` + `lld` (separate
formulae) locally, Debian's unversioned `clang lld llvm` in CI. The toolchain
file probes `/usr/local/opt/llvm` and `/opt/homebrew/opt/llvm` first and fails
at configure time naming whichever tool is missing. Wasm features are named
there rather than taken from the default CPU (which differs between clang 18 and
22): `-mreference-types` (else `__externref_t` is unknown), `-mbulk-memory`
(else `memcpy` is an undefined call), and
`-msign-ext -mmutable-globals -mnontrapping-fptoint`. The list is verified
sufficient by building over `-mcpu=mvp`.

Neither distribution provides compiler-rt for `wasm32-unknown-unknown`;
`-nostdlib` hides that until something needs a builtin (128-bit division, an
outlined `memcpy`).

Two link flags are deliberately **absent** and adding either back is a
regression: `--export-dynamic` (exports are named individually with
`BRAAM_EXPORT`) and `--allow-undefined` (without it an accidental libc
dependency is a link error, not a runtime trap). Concept.md §C.3.

### Verification

Three CTest cases, run on every build: `smoke` asserts the exact import/export
surface of `kernel.wasm` and of every binary, and that the kernel boots; `unit`
runs `tests.wasm` under Node; `size` holds `kernel.wasm` and the boot archive's
staging tree to `tools/size_budget.txt` — a program is measured but not bounded,
and adding a per-binary budget is a design change. New core code gets a case in
[test/unit/](test/unit/). Behind the three, M0–M9's acceptance criteria are the
standing behavioural contract: re-check by hand the ones a change touches.

Both wasm modules import the storage and service ABIs, so both are driven with
the in-memory backends in [test/fakefs.mjs](test/fakefs.mjs) and
[test/fakesvc.mjs](test/fakesvc.mjs), which answer from inside the import (no
browser can, and the kernel cannot tell — `wake()` only queues a resumption).
They take their constants, encoders and the archive unpacker (`parseZip`,
`installOps`) from `web/fs.js`, `web/svc.js` and `web/abi.js`; keep it that way
rather than restating the format — `run.mjs` parses `rootfs.zip` during its own
setup because inflating is asynchronous and every reply in the fake is not.
`tools/wsd.mjs` is a real WebSocket server, so `make serve` gives two tabs a
real conversation rather than a loopback.

## Architecture invariants

These three answer most "how should X work?" questions; full statements in
Concept.md §2.

1. **Coroutines are processes; the browser event loop is the scheduler.**
   Everything that would block becomes a `co_await`. No Asyncify, no JSPI, no
   threads, no stack switching. A suspended process is a coroutine frame in a
   hash map.
2. **A JS import never returns data — only accepts a wake token.** Results
   arrive through the `wake()` export. Exactly two exceptions are sanctioned:
   `host_now()` and OPFS sync access handles once a file is open
   (`host_fs_sync`); a third needs written justification in Concept.md. Storage
   and host services are multiplexed — one import per calling convention — so a
   new operation is an enum value on each side, not a new import.
   `ref(slot, obj)`, `sys` and `sys_async` are exports/process-side imports, not
   exceptions; spawning, stepping and killing a process are `host_svc`
   operations.
3. **The terminal is a cell grid in linear memory, not a byte stream.** No ANSI
   escapes, no VT100 emulation, no xterm.js. Colours are struct fields, cursor
   addressing is indexing. Mouse selection and copy live entirely on the page
   and in `web/render.js`, and the wheel becomes the scrollback chord's
   keystrokes in `web/worker.js`; there is no mouse anywhere in the ABI (§3.5).

Further constraints, easy to violate by habit:

- **No exceptions, no RTTI.** Errors are `Result<T, E>`, propagated with
  `TRY()`.
- **No `SharedArrayBuffer`**, therefore no COOP/COEP headers, therefore it hosts
  anywhere.
- **Every awaitable is cancellation-aware.** `CancelToken` participates in every
  `await_suspend`.
- **Coroutine frame allocation is the hot path**, and a frame past 512 bytes
  costs a whole 64 KiB span (the allocator's top size class). Long-lived state
  belongs in a heap block the frame points at. `test_shell` guards the shell's
  boot cost; it is why boot mounting is its own coroutine and why `FS_BLOCK` is
  512.
- **A host request may outlive the coroutine that issued it.** Anything whose
  address crosses to JS must be a heap record the kernel keeps alive past a
  cancelled await, never a frame buffer. `wake()` on an unclaimed token is what
  reaps one — that is why `sched_wake` returns a bool. A slot the host deposits
  into belongs to the record too: `reserve_ref()`, not a `JsRef` in the frame.
- **The externref table is the kernel's; JS never indexes it.**
  `import_module`/`import_name` do not apply to tables, so it is module-defined:
  the host deposits through the `ref` export and receives an object as an
  argument of `host_svc`.
- **Never `new` anything.** `operator new` returns null on failure and
  `-fno-exceptions` means the expression constructs at address zero. Use
  `heap_new`/`heap_delete` from `alloc.h`.
- **A namespace-scope global must be trivially destructible** — a non-trivial
  destructor pulls in `__cxa_atexit`, which nothing provides. Make the state a
  POD or put it behind a pointer built on first use (`Sched`). Constructors *do*
  run: `init()` calls `__wasm_call_ctors()` itself, **after** `heap_init`, so a
  constructor may allocate.
- **Every awaiter deregisters in its destructor.** `sched_unwait` from
  `~Awaiter` is what makes destroying a suspended frame safe. A parking
  awaitable with no destructor is a use-after-free.
- **A descriptor is held for the length of a syscall.** `Handle` is refcounted:
  `Close` frees the number and *shuts* what is behind it at once (answering a
  parked read) while the block and its externref slot wait for the last call.
  The slot must not recycle before then — `jsref_release` recycles it and
  `HostCall::issue()` re-reads it on a second attempt.
- **Whoever may still touch a `Stdio` holds a reference to what is behind it.**
  The three streams are function pointers into a block someone else owns, and
  cancellation is deferred, so a parked frame unwinds a tick after the stage
  that built it is gone. `Stdio::hold`/`owner` names that block; a syscall
  server takes a counted `ProcRef` **by value as a coroutine parameter**,
  because a parameter copy is destroyed after the body's locals — one of which
  deregisters from the pipe.
- **`memory.grow` detaches the `ArrayBuffer`**, killing cached `Uint8Array`
  views. Route JS-side access through a `view()` accessor that re-derives after
  growth.
- **A process binary shares headers with the kernel, not code.** `src/proc/`
  links `alloc.cpp`, `result.cpp`, `text.cpp`, `fs/path.cpp` and `braam_ui` and
  nothing else from the kernel's trees; anything reaching a host import shows up
  in the binary's import list, which `test/run.mjs` asserts per binary (as a
  subset — `true` imports no `sys_async` at all). That is why `panic` is
  declared in `host.h` and defined once per binary, and why it takes
  `(ptr, len)` rather than a `Str`: the wasm ABI passes an 8-byte struct
  indirectly, costing 2,812 bytes across the kernel.

### Keyboard, foreground and claims

- **One receiver per `Channel`, and the keyboard's is the console pump** —
  permanent, spawned by init (`src/user/console.h`), because something must hold
  the keyboard while nothing is running and a process has no `keys()`. A program
  claims a route through it (`KeyInput` in `src/user/tty.h`) rather than
  receiving on `keys()`, which would displace the pump silently and lose `^C`;
  the prompt is no exception. `CancelState::waiting` is one slot, so no task can
  be parked on a pipe and on the keyboard at once — which is why `less` reads
  its input to the end before it paints.
- **`^C` cancels the foreground if there is one and is delivered to the claimant
  if there is not.** The foreground is a set of pids armed with `Sys::Fg`; a
  shell arms its stages before it waits and arms nothing at a prompt, so the
  interrupt arrives as an ordinary key and abandons the line. Without that split
  a shell that is a process would be killed by its own `^C`.
- **The foreground belongs to whoever armed it** — the fourth clause of
  `Sys::Fg`'s authorisation, and the only one that lets a shell arm a
  *pipeline*: it releases the keyboard before it spawns, so from the second
  stage on it holds neither the keys nor a place in the set it is filling
  (§4.3). Nothing removes one pid from that set; the shell clears it after
  collecting and init clears it between shells.
- **0 is not a pid.** It is `sched_spawn`'s failure return, what
  `tty_keys_owner()` and `tty_screen_owner()` mean by "nobody", `SYS_WAIT_ANY`,
  `Fg(0)`, and `link.pid = 0` in `web/proc.js`. `/bin/sh` takes init's pid,
  since it is a process inside init's task rather than a job of its own, and
  `/proc/<init>` shows its cwd for the same reason.
- Each of the two routes (keys, screen) has **one holder, and a second claim is
  `Err(Perm)` rather than a nested one**, named by the pid that took it. A claim
  clears its route only if it is still the holder, so parent and child may die
  in either order. Nesting meant restoring a predecessor that may already be
  gone.
- The same rule is why `Sys::Spawn` **moves** a descriptor into the child rather
  than duplicating it — one end, one owner, so two blocked senders cannot be
  arranged from userland — and why one a syscall of the parent is parked on
  cannot be moved at all. Within a process, a second concurrent use of a
  descriptor in the same direction is `Err(Perm)`: on a pipe end
  `Channel::park_sender` panics and a second receiver is displaced silently, and
  `svc_blob`'s sized-twice reply is not re-entrant per object.

## Process model

Every program is a binary; there is no in-kernel program, no program registry
and no way to write one (Concept.md §4). A command word resolves as **function,
then builtin, then `/bin`**, and only the last of the three costs a process. A
file is a program when it carries the `braam` section, and otherwise when it
begins `#!` and names an absolute interpreter, which `exec_resolve` chases
exactly once; `test -x` answers from the same `exec_shebang` in `sysabi.h`. Two
kinds of thing run a command:

- **Shell builtin** — the twenty-six in `src/cmd/sh/builtin/`, inside `/bin/sh`,
  plus a shell function, which is the same thing named by the user. Two clauses
  make one. Either it touches the shell *process's* own state — its cwd, which a
  typed command inherits at spawn; its job table, which no syscall shows anyone;
  its variables, its options, its traps, its loop — **or its whole cost is the
  spawn**, which is `test`, `[`, `:`, `echo`, `true` and `false` and nothing
  else. The first kind has no file and never will; the second keeps its file in
  `/bin`, since a builtin shadows the name at a prompt and not everywhere. It
  pipes and redirects through descriptors like anything else, but runs **in its
  turn rather than alongside**, since nothing inside a process can wait for a
  sibling task — so it must buffer its output and write it once, or it fills an
  eight-slot pipe and parks with nobody left to drain it.
- **A process in a worker of its own** — address-space, capability, descriptor
  and memory-cap isolation plus a real kill switch, since wasm cannot be
  preempted. **This is what every program gets and the only thing one can get**:
  `braam_add_program` arranges it unasked, and the binary's `braam` section
  carries no placement word. One message each way per step; a syscall is two
  `postMessage` hops, 34–45 µs.

**A host with no worker to give is waited out, not worked around.** There is no
fallback: the spawn is refused with `Error::Again` and `spawn_process` in
`src/user/exec.cpp` backs off 10, 20, 50, 100, 200, 500 ms then a second
indefinitely, printing `no worker, retrying` on the program's own stderr. It is
an ordinary await, so `^C` abandons it. `hire()` latches nothing and tries
afresh every time, so a host that recovers is noticed.

**A process that loses its worker dies with it, and init replaces the shell.**
There is no moving a running process — the instance went with the worker. Init
starts another `/bin/sh` when its shell **died** (a trap, a failed step, an
instance that would not be made) and not when it **exited**, so `exit` still
ends the session; `exec_process`'s `bool *died` is what distinguishes them,
since `exit 132` and a trap are the same `i32`. Bounded at three deaths in quick
succession (a shell *waiting* for a worker is not one, since it has not
started). A replaced shell is a fresh one: kernel `/home`, empty job table.

**Whoever takes a worker away — `kill()` or `dropWorkers()` — must fail the
in-flight step**, or the kernel parks for ever on a reply that is not coming. An
abandoned `HostReq` is reaped by `wake()` on its token and by nothing else.

The kernel↔process ABI is Concept.md §4.3 and `src/kernel/sysabi.h`; both ends
include the header so neither can drift alone. Load-bearing rules:

- **The kernel never calls a process, and the host never calls one while the
  kernel is on the stack.** Only JS can call another instance's exports and a
  process calls straight back in through `sys`, so stepping one from inside a
  kernel import would run kernel code on a half-changed heap. A step is a
  `postMessage` (already an event-loop turn); in the test driver it is the link
  pumped between ticks. Synchronous syscalls go the other way and re-enter the
  kernel at top level, as `key()` does.
- **A process's pid is written into its import closure, not passed.** That is
  the whole of "a process cannot issue a syscall on behalf of another pid":
  there is no argument for it. The step protocol's messages carry *that* pid,
  never one read out of a message body.
- **A process may have several syscalls outstanding, and the step says which one
  it answers.** `PROC_TASKS` is 4 process-side; kernel-side each parked call is
  a `Call` record with its own staging block and its own scheduler job. One
  reused staging buffer would let the second call overwrite the first; one proxy
  serving them in turn would let a socket read that never completes starve the
  keystroke behind it. The resume token rides in the step request's `flags`.
- **A process's children are cancelled by its destructor** — §3.6's structured
  concurrency, by hand one level further down. A child is an ordinary scheduler
  job, so `^C`, `kill`, `jobs` and `/proc` reach it with nothing written for
  them; its status is recorded on the parent's record by a destructor that finds
  the parent by pid.
- **A pid is reused, but never while something still names it.** Both id spaces
  wrap, so the system has no lifetime; `sched_pid_hold`/`sched_pid_drop` is what
  makes that safe, held by the two things that outlive the job they name — an
  uncollected `Child` entry and a foreground entry. Anything else naming a pid
  either holds it only while the job runs or identifies its subject another way
  (the tty claims compare pointer identity), and adding a third holder means
  adding a hold. `SYS_PID_MAX` is 999999 and is the boundary between the two
  spaces, not the op word's 24-bit limit.
- **The scheduler has two job tables, and `/proc` lists one.** A task userland
  can address is named from `1..SYS_PID_MAX`; a job the kernel runs for itself
  is named above it and is absent from `sched_procs`, so it has no `/proc` line
  and `ps` needs no filter. Today that is the syscall server per parked call —
  the fire hose, ~150k a second through a bulk pipe, and a task `Wait`, `Kill`
  and `Fg` cannot name since all three search the caller's own children.
  `/proc/stat`'s gauges still count both, so `tasks` exceeds the row count of
  `/proc/tasks`.
- **A process ends when its root task returns**, whatever the others are doing.
  The kernel then drops the instance and cancels the servers of anything still
  outstanding.
- **Both halves of the step protocol live in `web/proc.js`** — `serveProc` is
  the process's side, `makeProc` the host's; `web/procworker.js` and
  `test/fakeworker.mjs` are wiring around them. Two files describing one wire is
  how it drifts.
- **The in-wasm unit tests cannot run a program at all**, and the shell is one:
  stepping an instance means returning to the host, and `run_tests()` does that
  once. `test/unit/` reaches everything *below* a program — the console and its
  claims, the pipes, `/proc`, the VFS, and the language (`parse.cpp`,
  `tokenize.cpp`, `expand.cpp`, `match.cpp` and `cond.cpp` are pure and compiled
  straight into the suite rather than linked from `braam_sh`, which would drag
  the process runtime's imports in — a syscall in any of the five is a link
  error, and that is what put the directory walk in `glob.cpp` and `test`'s file
  probes in `condrun.cpp`). Anything needing the shell or a program to actually
  run belongs in `test/run.mjs`.

A program is an ordinary scheduler job: a proxy task in `src/user/exec.cpp`
steps the instance and performs its syscalls with its own `CancelToken`, so
`^C`, `kill`, `jobs`, `/proc` and the stage epilogue need nothing added; its
destructor terminates the worker.

A pipeline's stages are independent scheduler jobs rather than a child group the
shell `co_await`s, because `CancelState::waiting` is a single slot. §3.6's
structured concurrency is put back by hand from a destructor in `run_line`'s
frame. `chat` shows the cost: a cancelled child does not unwind until the
scheduler resumes it, a tick or two after its parent is gone, so it must touch
nothing the parent owns. A real child-group awaitable needs intrusive queue
links inside `Waiter` first — the same work a channel with two blocked senders
would need, which `Channel::park_sender` panics on today rather than losing a
wakeup quietly.

## Known gaps

Absent on purpose, each for a reason in Release_Notes.md. None is a bug, and
adding one is a design change to argue in Concept.md first.

- **No `bg` and no `^Z`.** Stopping a running coroutine at an arbitrary point is
  the resume-side twin of `CancelToken` and would have to reach every awaitable.
- **Resize drops rows from the top rather than re-wrapping logical lines**,
  which §3.5 promised would arrive with scrollback. Scrollback arrived without
  it, and clips history rows the same way.
- **A mouse selection does not survive output.** Dropped by the next keystroke
  and by a resize; holding one needs the per-row continuation bit the re-wrap is
  waiting for. A selection *over* scrollback works, since the view is composed
  into the cells the renderer already reads.
- **Scrollback is 512 rows**, paged by half a screen (Shift+PageUp/PageDown) or
  moved a row at a time (Shift+Up/Down, which is what the page turns a wheel
  notch into); any key but the chord returns to the live screen. It is not a
  pager: no search, no mark, and no line model behind it.
- **A multi-line paste loses everything after the first command** — type-ahead
  across a command boundary, not the paste. Pasting one line is exact.
- **No per-process root.** A process has its own cwd, but an absolute path
  resolves with the kernel's full authority. A per-process mount view is a
  milestone's work in the VFS.
- **No file permissions, and `/bin` is writable.** OPFS stores no per-file mode
  and there is only one store, so `writable()` is per-mount and every mount but
  `/proc` is the one read-write store. `rm /bin/sh` is therefore reachable; what
  stands behind it is that `no_shell` offers to unpack `rootfs.zip` again. **The
  archive, not the store, is what the system recovers from.**
- **A directory has no modification time, and neither does `/proc`.** `Stat` and
  `Entry` carry an `mtime` in milliseconds, straight off the `File` that
  `getFile()` already yields, but OPFS puts no timestamp on a directory handle
  and a `/proc` file is generated at `open`. Both report 0, which is the
  system-wide "this filesystem does not know", and `ls -l` prints a dash rather
  than 1970. **There is no setter either**: `touch` moves a stamp only by
  rewriting the file with its own bytes and confirming the browser restamped it,
  and answers `Err(Unsupported)` when it did not. That confirmation is the only
  thing standing between a `touch` that works and one that lies, and no fake
  backend can test it — `make serve` in a real browser is where it is checked.
- **No system at all without OPFS.** Boot says so and starts no shell —
  deliberately, since the memory fallback that used to be here would look like a
  system and lose everything at the reload. It retires M5's third acceptance
  criterion; Release_Notes.md records that.
- **Ordinary output is not held to the screen claim.** `ScreenBlit` is refused
  without the claim, but a background job still writes to the grid through
  `stdout` and `ScreenClear` is open to anyone. Gating that is per-job output
  routing, not a claim.
- **No CPU metering.** A runaway program is killed; nothing bounds one.
- **`Pane` is a primitive, not a multiplexer.** One process at a time holds the
  screen; a second `ScreenEnter` is `Err(Perm)`.
- **Every command costs an instantiation and a worker** — roughly a millisecond,
  plus reading the image out of the store. The host caches the compiled `Module`
  by path, so only the compile is saved; the worker comes from the pool. A `#!`
  script costs one, not two, and shares the interpreter's cached `Module` — but
  its image is read twice, once by the kernel for its first line and once by the
  interpreter.
- **Every syscall a program makes is two `postMessage` hops**, 34–45 µs. Bulk
  I/O pays it per `SYS_CHUNK` (512 bytes): measured at 6–13 ms over a quarter of
  a megabyte through three processes, which is why a bigger chunk or a batched
  step protocol was decided against. A workload moving megabytes is what would
  reopen it.
- **A keystroke at the prompt costs two round trips** (`key_read`, then the
  `echo` that repaints) and Enter to the next prompt costs five (`echo`,
  newline, `cwd_get`, prompt `echo`, `key_read`). Both are floors: the cwd is
  deliberately not cached, since a wrong prompt is believed, and going lower
  means fusing the keyboard into the paint — a worse ABI than it would save.
- **The boot archive is ~710 KiB unpacked** and 287 KiB as `rootfs.zip`. §4.4's
  duplication: every binary carries the allocator, the string types and the
  coroutine runtime, and `sh.wasm` is 214 KiB of it — nearly a third of the
  tree, since the shell is a language (§4.5). The staging *tree* carries the
  size budget and the binaries do not, so that number is where the duplication
  stays visible — the archive is deflated and would hide it.
- **A soft keyboard has no control keys.** `^C`/`^D`/`^L`, `Esc`, `Tab` and the
  arrows reach a tablet only through the page's key bar (`mount({keys})`);
  widening it is a page change, not a system one. The focus lives on a hidden
  `<textarea>`, not the canvas — a canvas is focusable but not editable, and
  only an editable element raises a keyboard.
- **`kill <pid>` is gone; `kill %n` is not.** `Sys::Kill` refuses anything not a
  child of the caller.
- **No `/proc/jobs`.** The job table is the shell process's own memory. The
  stages are still tasks, so `/proc/<pid>` lists them — which is how the shell
  notices a background job finished.
- **`$$` is not unique per shell.** It is `proc_pid()`, and a top-level
  `/bin/sh` reports init's, since it is a process inside init's task rather than
  a job of its own. A nested `sh` gets one of its own. Nothing derives a
  filename from it, so nothing collides; `$$` is not a temp-file scheme here,
  because there are no temp files.
- **No `setenv`: a process's environment is fixed at spawn.** `export` does
  reach a child now — `Sys::Spawn` carries an env blob after the argv one, the
  kernel keeps it on `Proc` beside the cwd, and a spawn naming none hands the
  child the caller's. But a process cannot change its own: there is no
  `Sys::Env`, because nothing would call one (the shell builds the blob from its
  own table at each spawn), and adding one would break §4.3's "every operation
  has a caller in `src/cmd/`". `x=1 prog` reaches that child alone;
  `/bin/export` is still named `save`, to give the builtin its name.
- **A script file is parsed whole.** `sh file` reads and parses it at once, as
  `.` does, so a syntax error anywhere in it means none of it runs — v7 runs
  everything above the error. `#!` works now (`./script.sh`), within three
  bounds: the interpreter must be absolute (there is no PATH), the lookup is one
  level deep, and the first line must end within `PROC_SHEBANG_MAX`.
- **`trap` has two signals and one of them cannot be ignored.** There are none
  in the system, so `trap … 0` (EXIT) and `trap … 2` (INT) are all there is, and
  `trap '' 2` is refused rather than accepted and dropped:
  `CancelState::cancelled` is sticky. A `trap … 2` in a *script* shell can never
  fire, because the process itself is cancelled and every await after that
  answers `Err(Cancelled)`.
- **`read` reads past the line it was asked for.** `Sys::Read` carries no
  length, so a chunk is whatever the writer wrote; what followed the newline is
  kept in a pushback buffer keyed by the descriptor. `sh -s` has a `LineReader`
  of its own, so a `read` inside a script off stdin sees a different position in
  the same stream.
- **A function is not a scope.** It runs in the shell's own turn on the caller's
  `Ctx`, so `$1`…`$#` are saved and put back but its variables and cwd are the
  shell's, and a `break` inside one reaches a loop outside it. `f | wc` and
  `f > log` work: the body inherits the stage's descriptors through `Ctx::base`.
- **Globbing is argv words only**, not a redirection target or an assignment
  value: both expand under `Split::One`, one field in and one out, and `> *.txt`
  writes to the pattern rather than silently to whatever it matched. An
  unterminated `[` matches nothing (v7's answer), and a quoted leading dot still
  counts as one, so `\.*` finds dotfiles where v7 finds none.
- **`( … )` isolates state, not memory.** There is no `fork`, so a subshell runs
  in this process with the shell's own mutable state saved and put back around
  it (Concept.md §4.5). `(cd /x; ls)` and `(set -e; …)` are exact; a `hog`
  inside one still spends the shell's 16 MB, and a `$( )` inside one is bounded
  by the same cap.
- **`(` and `)` are tokens with no grammar above a subshell and a `case` arm.**
  `echo (x)` is `syntax error near '('` rather than a word, which is what `)`
  ending an arm cost.
- **`exec >file` cannot be undone, and `>&-` cannot be said.** There is no
  `/dev/tty` and no way to name the stream the shell was handed, so a top-level
  `exec` redirection is permanent; inside `( … )` it is checkpointed like
  everything else. `>&-` would need a closed-descriptor value in `Sys::Spawn`'s
  payload and a null *sink* in the kernel, and there is neither.
- **`( … )` restores everything but the job table**, which is deliberately
  shared: its children are this same process's and must still be reaped. cwd,
  variables, positional parameters, the function table and the `exec` base all
  go back.
- **A `$( )` is not a subshell, and unlike `( … )` it takes no checkpoint.** It
  runs in the shell, so `$(cd /x)` moves it and `$(y=1)` sets a real variable;
  `$(exit)` does *not* end the shell, since the command is run against its own
  `Ctx` rather than through `run_line`. The save-and-restore checkpoint is
  `( … )`'s alone. Its output is bounded only by the process's 16 MB cap — v7's
  answer too, and truncating would corrupt a value silently.
- **Only a pipeline may go into the background.** `a && b &`, `{ … ; } &` and
  `while … done &` are refused, because backgrounding means the shell keeps
  running while the rest goes and nothing in a process can wait for a sibling
  task. **And a compound command cannot be *piped*** — `{ a; } | wc` — because a
  `Pipe` node's stages are commands and making them nodes is its own change.
  Redirecting one works: `{ a; } > f` and `for … done > f` ride on `Ctx::base`.
- **A loop whose body is entirely builtins cannot be interrupted, and
  `while true; do echo …; done` is now one of those.** The shell arms its
  *children* with `Sys::Fg` and is never in its own foreground set, so a `^C`
  has nowhere to go and `rt.h` is explicit that nothing cancels from inside a
  process. Making `true`, `echo` and `test` builtins widened this: the case that
  used to be interrupted in one press is the case that now cannot be. A loop
  with a program in it — `while true; do sleep 100; done` — still is. The escape
  from the other kind is killing the shell, which init then replaces.
- **Two fidelity losses the worker costs (§4.3):** a binary that will not
  instantiate reads as a crash rather than a refusal, and `Sys::Now` is relative
  (nothing calls `proc_now()`).
- **A host that cannot make a nested worker cannot run anything**, and says so
  once a second rather than degrading — there is no weaker place to put a
  process, so `/bin/sh` itself waits at boot. A worker made that *then* fails to
  load is not retried: the spawn has been answered, so it reads as a process
  that died and init's three respawns bound it.

## Conventions

- **Keep comments in code and scripts terse.** A comment says what a thing is,
  not why the design is the way it is; reasoning and trade-offs go in
  [doc/Release_Notes.md](doc/Release_Notes.md).
- Commits: no `Co-Authored-By` trailer, no generated-with footer. Commit only
  when asked.
- Layout (Concept.md §7): `src/kernel/`; `src/fs/` (paths, VFS, filesystems,
  host storage ABI); `src/svc/` (fetch, WebSocket, clipboard, file transfer,
  wall clock, process operations); `src/ui/` (layout over a `Grid`: `Pane`,
  `TextBuf`, `TextView`); `src/user/` (`exec` and the syscall dispatcher, the
  console and its pump, the pipes behind a stage's stdio, `ProcFs`, boot and
  init); `src/proc/` (a process binary's runtime, `screen.cpp` included);
  `src/cmd/` (one file per program) and `src/cmd/sh/` (the one program that
  needs a directory: grammar, word expander, pattern matcher, condition
  evaluator, variables, line editor, job runtime, builtins, and the `main.cpp`
  that makes them a binary); `test/unit/`; `web/`; `rootfs/`; `examples/`;
  `tools/`; `cmake/`.
- `braam_fs` and `braam_svc` are siblings above the kernel and below userland:
  they must not depend upwards or on each other, and anything needing the
  scheduler or screen belongs in `src/user/` (which is why `ProcFs` lives
  there). **`braam_sh` links `braam_proc`**, so nothing in it may reach a kernel
  header that pulls in the scheduler. **`braam_ui` links `braam_flags` alone**
  and the kernel does not link it at all; keep it clear of the VFS, the screen
  and every host import. The exact-import assertion over each binary is what
  says otherwise.
- **The builtin table is an explicit array and must stay one.** `--gc-sections`
  never extracts an unreferenced archive member, so a self-registering builtin
  would be dropped without a word.
- Exports are declared with `BRAAM_EXPORT("name")`, imports with
  `BRAAM_IMPORT("name")` — never by linker flag. Either changes the ABI, so
  update the expected surface in [test/run.mjs](test/run.mjs) in the same
  commit.
- `.clang-format` at the root is authoritative: 4-space indent, 100 columns.
  Types `PascalCase`, functions and variables `snake_case`, constants
  `SCREAMING_SNAKE`.
- Markdown wraps at 80 columns (`.editorconfig`). Prose and list items are
  rewrapped; tables, fenced and indented code blocks, and headings are left
  alone, so a long one of those stays long.

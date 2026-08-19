# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository.

## What this project is

Braam is a CLI-oriented operating system running entirely in a browser tab:
kernel, shell, filesystem, terminal and programs, in freestanding C++20 compiled
to WebAssembly, deployable as a static site with no server and no special HTTP
headers.

Nothing below may regress: `kernel.wasm` against its 256 KiB budget, the boot
archive's staging tree against 1 MiB, a wasm ABI of six imports and nine
exports, three passing CTest cases.

## Documents

- **[doc/Concept.md](doc/Concept.md) is the specification.** Read before
  anything substantive; amend in the same commit as the code. Its section
  numbers are cited from source: do not renumber. `§n` below refers to it.
- **[doc/Release_Notes.md](doc/Release_Notes.md) is where the *why* goes** —
  appended under a new heading, never by rewriting a section. Also holds M0–M9's
  objectives and twenty-two acceptance criteria, which are live constraints.
  Read its M0 section before touching the allocator, the coroutine shim or the
  build flags.
- **[doc/System_Calls.md](doc/System_Calls.md)** derives the kernel↔process
  mechanism (§4.3 is normative). Changes to `src/proc/`, `src/user/`,
  `src/kernel/sysabi.h` or `web/proc.js` must keep it true.
- **[doc/Programming_Manual.md](doc/Programming_Manual.md)** is the SDK guide.
- **[doc/Package_Management.md](doc/Package_Management.md)** is the policy
  `/bin/pkg` must satisfy; the trust model changes there first. `/bin/pkg` does
  not exist yet.

## Build

CMake with a toolchain file, Unix Makefiles; clang, cmake, make, node, no ninja.
The top-level `Makefile` wraps it and configures on first use:

```
make            # build kernel.wasm, the /bin binaries and tests.wasm
make run        # ctest
make serve      # serve build/web/ and open a browser
make install    # the SDK, to /usr/local if writable else ~/.local
make release    # pack build/web/ and the SDK as build/*.zip
make clean      # rm -rf build
```

- Overrides: `JOBS`, `GENERATOR=Ninja`, `BUILD=<dir>`, `PREFIX=<dir>`. **`make
  -jN` does not reach the compiler** — change `JOBS`. `CMAKE_ARGS` reaches only
  the configure step.
- The always-run `web` target uses `copy_directory`, which never deletes. Cut a
  release from a clean tree.
- Version = `BRAAM_VERSION_BASE` (`src/kernel/version.h`, hand-edited) + commit
  count + short hash. `tools/version.py` is the one implementation and runs at
  *build* time; `tools/release.py` imports it and packs reproducibly.
- The SDK is `make install` plus that second archive: headers, `libbraam_proc.a`
  and `libbraam_ui.a` and no other library (the rest carry host imports), the
  toolchain file, `stamp.py`, a relocatable CMake package. `braam_flags`/the
  toolchain must keep supplying `cxx_std_20`, `MinSizeRel` (at `-O0` a
  freestanding build needs libcalls nothing provides) and `-Werror` behind
  `$<BUILD_INTERFACE:>`.
- `braam_add_program(NAME … SOURCES … [LIBS])` in `cmake/BraamProgram.cmake` is
  shared by `src/cmd/` and the SDK; `examples/hello/` is a build target too.
- `-Wall -Wextra -Wshadow`, `BRAAM_WERROR` **ON by default**, tree is
  warning-clean. `-DBRAAM_WERROR=OFF` is for bisecting only.

### Toolchain

- Clang only, and **nothing from its runtime or headers is linked or included**:
  `-nostdinc++` is mandatory, `--no-default-config` keeps a config file from
  injecting a sysroot. libc++'s `<coroutine>` is unusable freestanding;
  [src/kernel/coroutine.h](src/kernel/coroutine.h) shims `__builtin_coro_*`.
- Any clang with the wasm32 target; the toolchain file probes
  `/usr/local/opt/llvm` and `/opt/homebrew/opt/llvm` and fails at configure time
  naming the missing tool. There is no compiler-rt for
  `wasm32-unknown-unknown`.
- Wasm features are named explicitly, not taken from the default CPU:
  `-mreference-types` (else `__externref_t` is unknown), `-mbulk-memory` (else
  `memcpy` is undefined), `-msign-ext -mmutable-globals -mnontrapping-fptoint`.
- Two link flags are deliberately **absent**; re-adding either is a regression
  (§C.3): `--export-dynamic` (use `BRAAM_EXPORT`) and `--allow-undefined`
  (without it an accidental libc dependency is a link error).

### Verification

Three CTest cases on every build: `smoke` asserts the exact import/export
surface of `kernel.wasm` and every binary, and that the kernel boots; `unit`
runs `tests.wasm` under Node; `size` holds `kernel.wasm` and the staging tree to
`tools/size_budget.txt`. New core code gets a case in [test/unit/](test/unit/).

Both wasm modules are driven by the in-memory backends
[test/fakefs.mjs](test/fakefs.mjs) and [test/fakesvc.mjs](test/fakesvc.mjs),
which answer from inside the import. They take their constants, encoders and
archive unpacker from `web/fs.js`, `web/svc.js`, `web/abi.js` — do not restate
the format. `tools/wsd.mjs` is a real WebSocket server.

## Architecture invariants

Full statements in §2.

1. **Coroutines are processes; the browser event loop is the scheduler.**
   Everything blocking becomes a `co_await`. No Asyncify, no JSPI, no threads,
   no stack switching. A suspended process is a coroutine frame in a hash map.
2. **A JS import never returns data — only accepts a wake token.** Results
   arrive through `wake()`. Two sanctioned exceptions: `host_now()` and OPFS
   sync access handles once a file is open (`host_fs_sync`); a third needs
   written justification in Concept.md. Storage and host services are
   multiplexed — one import per calling convention — so a new operation is an
   enum value on each side, not a new import. `ref(slot, obj)`, `sys` and
   `sys_async` are exports/process-side imports, not exceptions; spawning,
   stepping and killing a process are `host_svc` operations.
3. **The terminal is a cell grid in linear memory, not a byte stream.** No ANSI
   escapes, no VT100, no xterm.js. Colours are struct fields, cursor addressing
   is indexing. Mouse selection and copy live on the page and in
   `web/render.js`; there is no mouse anywhere in the ABI (§3.5).

Further constraints, easy to violate by habit:

- **No exceptions, no RTTI.** Errors are `Result<T, E>`, propagated with `TRY()`.
- **No `SharedArrayBuffer`**, hence no COOP/COEP headers, hence it hosts
  anywhere.
- **Every awaitable is cancellation-aware.** `CancelToken` participates in every
  `await_suspend`.
- **Coroutine frame allocation is the hot path.** A frame past 512 bytes costs a
  whole 64 KiB span; long-lived state belongs in a heap block the frame points
  at. `FS_BLOCK` is 512 for the same reason.
- **A host request may outlive the coroutine that issued it.** Anything whose
  address crosses to JS must be a heap record kept alive past a cancelled await,
  never a frame buffer; `wake()` on an unclaimed token reaps one, which is why
  `sched_wake` returns a bool. A slot the host deposits into belongs to the
  record: `reserve_ref()`, not a `JsRef` in the frame.
- **The externref table is the kernel's; JS never indexes it.** The host
  deposits through the `ref` export and receives objects as `host_svc` arguments.
- **Never `new` anything** — `operator new` returns null on failure and
  `-fno-exceptions` constructs at address zero. Use `heap_new`/`heap_delete`.
- **A namespace-scope global must be trivially destructible** — a non-trivial
  destructor pulls in `__cxa_atexit`. Make it a POD or hide it behind a pointer
  built on first use (`Sched`). Constructors *do* run: `init()` calls
  `__wasm_call_ctors()` **after** `heap_init`, so a constructor may allocate.
- **Every awaiter deregisters in its destructor** (`sched_unwait` from
  `~Awaiter`); a parking awaitable with no destructor is a use-after-free.
- **A descriptor is held for the length of a syscall.** `Handle` is refcounted:
  `Close` frees the number and *shuts* what is behind it at once while the block
  and its externref slot wait for the last call.
- **Whoever may still touch a `Stdio` holds a reference to what is behind it**
  (`Stdio::hold`/`owner`). A syscall server takes a counted `ProcRef` **by value
  as a coroutine parameter**, so the copy outlives the body's locals.
- **`memory.grow` detaches the `ArrayBuffer`**, killing cached `Uint8Array`
  views. Route JS-side access through a `view()` accessor.
- **A process binary shares headers with the kernel, not code.** `src/proc/`
  links `alloc.cpp`, `result.cpp`, `text.cpp`, `fs/path.cpp` and `braam_ui` and
  nothing else from the kernel's trees; `test/run.mjs` asserts each binary's
  import list. Hence `panic` is declared in `host.h`, defined once per binary,
  and takes `(ptr, len)` rather than a `Str`.

### Keyboard, foreground and claims

- **One receiver per `Channel`; the keyboard's is the console pump** —
  permanent, spawned by init (`src/user/console.h`). A program claims a route
  through it (`KeyInput` in `src/user/tty.h`) rather than receiving on `keys()`,
  which would displace the pump silently and lose `^C`; the prompt included.
  `CancelState::waiting` is one slot, so no task can be parked on a pipe and on
  the keyboard at once.
- **`^C` cancels the foreground if there is one, and reaches the claimant if
  there is not.** The foreground is a set of pids armed with `Sys::Fg`; a shell
  arms its stages before it waits and arms nothing at a prompt.
- **The foreground belongs to whoever armed it** — `Sys::Fg`'s fourth
  authorisation clause, the only one that lets a shell arm a *pipeline* (§4.3).
  Nothing removes one pid from the set; the shell clears it after collecting,
  init between shells.
- **0 is not a pid.** It is `sched_spawn`'s failure return, "nobody" for the tty
  owners, `SYS_WAIT_ANY`, `Fg(0)`, and `link.pid = 0` in `web/proc.js`.
  `/bin/sh` takes init's pid, being a process inside init's task.
- Each route (keys, screen) has **one holder; a second claim is `Err(Perm)`**. A
  claim clears its route only if it is still the holder, so parent and child may
  die in either order.
- `Sys::Spawn` **moves** a descriptor into the child rather than duplicating it,
  and one a syscall of the parent is parked on cannot be moved at all. A second
  concurrent use of a descriptor in the same direction is `Err(Perm)`.

## Process model

Every program is a binary; there is no in-kernel program, no registry, no way to
write one (§4). A command word resolves as **function, then builtin, then
`PATH`**, and only the last costs a process. A file is a program when it carries
the `braam` section, otherwise when it begins `#!` and names an absolute
interpreter, which `exec_resolve` chases exactly once; `test -x` answers from
the same `exec_shebang` in `sysabi.h`.

- **Shell builtin** — the twenty-six in `src/cmd/sh/builtin/`, plus shell
  functions. A builtin either touches the shell *process's* own state (cwd, job
  table, variables, options, traps, loop, and the tables `command -v` reads) —
  **or its whole cost is the spawn**, which is `test`, `[`, `:`, `echo`, `true`,
  `false` and nothing else. The first kind has no file; the second keeps its
  file in `/bin`, since a builtin shadows the name at a prompt and not
  everywhere. It pipes and redirects through descriptors, but runs **in its turn
  rather than alongside**, so it must buffer its output and write it once or it
  fills an eight-slot pipe with nobody to drain it.
- **A process in a worker of its own** — address-space, capability, descriptor
  and memory-cap isolation plus a real kill switch. **This is what every program
  gets and the only thing one can get**: `braam_add_program` arranges it unasked
  and the `braam` section carries no placement word.

**`PATH` is searched by the kernel, not the shell.** `exec_resolve` in
`src/user/exec.cpp` reads the word out of the env blob the spawn carries.
Components are `:`-separated; an empty one is skipped; a relative one resolves
against the caller's cwd; no `PATH` means `SYS_PATH_DEFAULT` (`/bin`) while an
empty one finds nothing. A candidate that is not a program does not shadow one
further along, and a search that found only those is `Err(Invalid)` (126) rather
than `Err(NotFound)` (127). The shell marks `PATH` exported on every assignment.

**A host with no worker to give is waited out, not worked around.** The spawn is
refused with `Error::Again` and `spawn_process` backs off; it is an ordinary
await, so `^C` abandons it.

**A process that loses its worker dies with it, and init replaces the shell.**
Init starts another `/bin/sh` when its shell **died** and not when it
**exited**; `exec_process`'s `bool *died` distinguishes them. Bounded at three
deaths in quick succession. A replaced shell is fresh: kernel `/home`, empty job
table.

**Whoever takes a worker away — `kill()` or `dropWorkers()` — must fail the
in-flight step**, or the kernel parks for ever. An abandoned `HostReq` is reaped
by `wake()` on its token and by nothing else.

The kernel↔process ABI is §4.3 and `src/kernel/sysabi.h`; both ends include the
header. Load-bearing rules:

- **The kernel never calls a process, and the host never calls one while the
  kernel is on the stack.** A step is a `postMessage`; in the test driver it is
  the link pumped between ticks. Synchronous syscalls go the other way and
  re-enter the kernel at top level.
- **A process's pid is written into its import closure, not passed** — that is
  the whole of "a process cannot issue a syscall on behalf of another pid".
- **A process may have several syscalls outstanding; the step says which one it
  answers.** `PROC_TASKS` is 4 process-side; kernel-side each parked call is a
  `Call` record with its own staging block and scheduler job. The resume token
  rides in the step request's `flags`.
- **A process's children are cancelled by its destructor** — §3.6's structured
  concurrency by hand. A child is an ordinary scheduler job, so `^C`, `kill`,
  `jobs` and `/proc` reach it; its status is recorded on the parent's record.
- **A pid is reused, but never while something still names it.** Both id spaces
  wrap; `sched_pid_hold`/`sched_pid_drop` makes that safe, held by an
  uncollected `Child` entry and a foreground entry. A third holder means adding
  a hold. `SYS_PID_MAX` is 999999 and is the boundary between the two spaces.
- **The scheduler has two job tables and `/proc` lists one.** Userland-
  addressable tasks are `1..SYS_PID_MAX`; a job the kernel runs for itself is
  named above it, is absent from `sched_procs`, and `Wait`/`Kill`/`Fg` cannot
  name it — today that is the syscall server per parked call. `/proc/stat`'s
  gauges count both.
- **A process ends when its root task returns**, whatever the others are doing;
  the kernel drops the instance and cancels the outstanding servers.
- **Both halves of the step protocol live in `web/proc.js`** — `serveProc` the
  process's side, `makeProc` the host's; `web/procworker.js` and
  `test/fakeworker.mjs` are wiring around them.
- **The in-wasm unit tests cannot run a program at all**, and the shell is one.
  `test/unit/` reaches everything *below* a program — console and claims, pipes,
  `/proc`, VFS, and the language (`parse.cpp`, `tokenize.cpp`, `expand.cpp`,
  `match.cpp`, `cond.cpp` are pure and compiled straight into the suite rather
  than linked from `braam_sh`, so a syscall in any of the five is a link error).
  Anything needing a program to run belongs in `test/run.mjs`.

A program is an ordinary scheduler job: a proxy task in `src/user/exec.cpp`
steps the instance and performs its syscalls with its own `CancelToken`; its
destructor terminates the worker.

A pipeline's stages are independent scheduler jobs rather than a child group the
shell `co_await`s, because `CancelState::waiting` is a single slot; §3.6's
structured concurrency is put back by hand from a destructor in `run_line`'s
frame. A cancelled child does not unwind until the scheduler resumes it, so it
must touch nothing the parent owns.

## Known gaps

Absent on purpose, each with a reason in Release_Notes.md. None is a bug; adding
one is a design change to argue in Concept.md first.

- **No `bg`, no `^Z`.** Stopping a running coroutine would reach every awaitable.
- **Resize drops rows from the top rather than re-wrapping logical lines**, and
  a mouse selection does not survive output, a keystroke or a resize — both wait
  on a per-row continuation bit.
- **Scrollback is 512 rows**, paged by Shift+PageUp/PageDown or Shift+Up/Down;
  any other key returns to the live screen. No search, no mark, no line model.
- **A multi-line paste loses everything after the first command.**
- **`help` is a file, so it lists what ships and not what `PATH` holds.**
  `/bin/help` is `#!/bin/sh` over `less /share/help`. `less` off a terminal
  copies rather than pages, which keeps `help | grep ls` working.
- **No per-process root.** A process has its own cwd, but an absolute path
  resolves with the kernel's full authority.
- **No file permissions, and `/bin` is writable.** OPFS stores no per-file mode
  and there is one store, so `writable()` is per-mount. `rm /bin/sh` is
  reachable; `no_shell` offers to unpack `rootfs.zip` again. **The archive, not
  the store, is what the system recovers from.**
- **Symbolic links only, no hard links.** A link is a store file whose whole
  contents are `!<braamlink>` and the target (§5.2); `ln` without `-s` refuses.
  Resolution is `vfs_resolve`'s alone — a store reports `NodeKind::Link` and
  never follows one — and is *lazy*. **A listing never resolves**, so `ls -R`
  and the globber need no cycle guard. `..` stays **lexical** (`cd -L`), because
  `path_resolve` must stay synchronous for `proc_path`. Bounded at
  `FS_LINK_MAX` hops with `Error::Loop`. `tools/pack.py` carries no link into
  `rootfs.zip`; `ls -l` prints a target and there is no `/bin/readlink`.
- **A directory has no mtime, and neither does `/proc`** — both report 0, the
  system-wide "unknown". **No setter either**: `touch` rewrites the file with
  its own bytes and confirms the browser restamped it, answering
  `Err(Unsupported)` when it did not — untestable by any fake backend, so
  `make serve` in a real browser is where it is checked.
- **A rename is only sometimes a rename**, and `mv` copies for the rest.
  `Sys::Rename` is backed by OPFS `FileSystemHandle.move()`, which works on a
  *file* handle alone and not in every engine, so a directory, a cross-mount
  move and a missing method all answer `Err(Unsupported)` — which `/bin/mv`
  alone reads as "copy and remove instead", not atomically. `vfs_rename` refuses
  ahead of the round trip (a mount point, an open descriptor on the source, a
  directory into itself, disagreeing kinds) and answers a move onto the same
  file with `Ok` and nothing done.
- **No system at all without OPFS.** Boot says so and starts no shell.
- **Ordinary output is not held to the screen claim.** `ScreenBlit` is refused
  without the claim, but a background job still writes to the grid through
  `stdout` and `ScreenClear` is open to anyone.
- **No CPU metering.** A runaway program is killed; nothing bounds one.
- **`Pane` is a primitive, not a multiplexer.** One process at a time holds the
  screen; a second `ScreenEnter` is `Err(Perm)`.
- **Every command costs an instantiation and a worker.** The host caches the
  compiled `Module` by path; the worker comes from the pool. A `#!` script costs
  one worker, not two, but its image is read twice.
- **Every syscall is two `postMessage` hops**, and bulk I/O pays that per
  `SYS_CHUNK` (512 bytes); the cwd behind the prompt is likewise not cached. A
  bigger chunk or a batched step protocol was decided against.
- **Every binary carries the allocator, the string types and the coroutine
  runtime** (§4.4). The staging *tree* carries the size budget, not the binaries.
- **A soft keyboard has no control keys.** `^C`/`^D`/`^L`, `Esc`, `Tab` and the
  arrows reach a tablet only through the page's key bar (`mount({keys})`).
  Focus lives on a hidden `<textarea>`, not the canvas.
- **`kill <pid>` is gone; `kill %n` is not.** `Sys::Kill` refuses anything not a
  child of the caller.
- **No `/proc/jobs`.** The job table is the shell process's own memory; the
  stages are still tasks, so `/proc/<pid>` lists them.
- **`$$` is not unique per shell.** It is `proc_pid()`, and a top-level
  `/bin/sh` reports init's.
- **`command` is the query half only.** `command -v` works; a bare
  `command <cmd>` is a usage line and 2.
- **No `setenv`: a process's environment is fixed at spawn.** `Sys::Spawn`
  carries an env blob after the argv one, the kernel keeps it on `Proc` beside
  the cwd, and a spawn naming none hands the child the caller's. There is no
  `Sys::Env`. `/bin/export` is named `save`.
- **A script file is parsed whole**, so a syntax error anywhere means none of it
  runs. `#!` works within three bounds: absolute interpreter, one level of
  lookup, first line within `PROC_SHEBANG_MAX`.
- **`trap` has two signals and one cannot be ignored.** Only `trap … 0` (EXIT)
  and `trap … 2` (INT) exist; `trap '' 2` is refused, since
  `CancelState::cancelled` is sticky. A `trap … 2` in a *script* shell can never
  fire.
- **`read` reads past the line it was asked for.** `Sys::Read` carries no
  length, so what followed the newline is kept in a pushback buffer keyed by the
  descriptor. `sh -s` has its own `LineReader`.
- **A function is not a scope.** It runs in the shell's own turn on the caller's
  `Ctx`: `$1`…`$#` are saved and restored, but its variables and cwd are the
  shell's and a `break` inside reaches a loop outside. `f | wc` and `f > log`
  work through `Ctx::base`.
- **Globbing is argv words only**, not a redirection target or an assignment
  value (both expand under `Split::One`), so `> *.txt` writes to the pattern.
- **`( … )` isolates state, not memory.** There is no `fork`; a subshell runs in
  this process with mutable state saved and restored around it (§4.5),
  everything **but the job table** — shared, since its children are this
  process's and must still be reaped.
- **`(` and `)` are tokens with no grammar above a subshell and a `case` arm.**
- **`exec >file` cannot be undone, and `>&-` cannot be said.** There is no
  `/dev/tty` and no way to name the stream the shell was handed, so a top-level
  `exec` redirection is permanent; inside `( … )` it is checkpointed.
- **A `$( )` is not a subshell and takes no checkpoint.** It runs in the shell,
  so `$(cd /x)` moves it and `$(y=1)` sets a real variable; `$(exit)` does not
  end the shell. Its output is bounded only by the 16 MB cap.
- **Only a pipeline may go into the background**; `a && b &`, `{ … ; } &` and
  `while … done &` are refused. **And a compound command cannot be *piped***,
  since a `Pipe` node's stages are commands. Redirecting one works, through
  `Ctx::base`.
- **A loop whose body is entirely builtins cannot be interrupted.** The shell
  arms its *children* with `Sys::Fg` and is never in its own foreground set, and
  nothing cancels from inside a process (`rt.h`). The escape is killing the
  shell.
- **Two fidelity losses the worker costs (§4.3):** a binary that will not
  instantiate reads as a crash rather than a refusal, and `Sys::Now` is relative.
- **A host that cannot make a nested worker cannot run anything**, and says so
  rather than degrading; `/bin/sh` itself waits at boot.

## Conventions

- **Keep comments in code and scripts terse.** A comment says what a thing is,
  not why; reasoning goes in [doc/Release_Notes.md](doc/Release_Notes.md).
- Commits: no `Co-Authored-By` trailer, no generated-with footer. Commit only
  when asked.
- Layout (§7): `src/kernel/`; `src/fs/` (paths, VFS, filesystems, host storage
  ABI); `src/svc/` (fetch, WebSocket, clipboard, file transfer, wall clock,
  process operations); `src/ui/` (layout over a `Grid`: `Pane`, `TextBuf`,
  `TextView`); `src/user/` (`exec` and the syscall dispatcher, console and pump,
  the pipes behind a stage's stdio, `ProcFs`, boot and init); `src/proc/` (a
  process binary's runtime, `screen.cpp` included); `src/cmd/` (one file per
  program) and `src/cmd/sh/` (grammar, expander, matcher, condition evaluator,
  variables, line editor, job runtime, builtins, `main.cpp`); `test/unit/`;
  `web/`; `rootfs/`; `examples/`; `tools/`; `cmake/`.
- `braam_fs` and `braam_svc` are siblings above the kernel and below userland:
  no upward dependency and none on each other; anything needing the scheduler or
  screen belongs in `src/user/` (hence `ProcFs`). **`braam_sh` links
  `braam_proc`**, so nothing in it may reach a kernel header pulling in the
  scheduler. **`braam_ui` links `braam_flags` alone** and the kernel does not
  link it; keep it clear of the VFS, the screen and every host import.
- **The builtin table is an explicit array and must stay one.** `--gc-sections`
  never extracts an unreferenced archive member, so a self-registering builtin
  would be dropped silently.
- **A new program or builtin updates `rootfs/share/help` in the same commit.**
  That document is the whole of `help`, and nothing at run time notices when it
  goes stale — [test/run.mjs](test/run.mjs) checks it against the builtin table
  and the archive's `bin/`, so a forgotten line is a failing `smoke`.
- Exports are declared with `BRAAM_EXPORT("name")`, imports with
  `BRAAM_IMPORT("name")` — never by linker flag. Either changes the ABI: update
  the expected surface in [test/run.mjs](test/run.mjs) in the same commit.
- `.clang-format` at the root is authoritative: 4-space indent, 100 columns.
  Types `PascalCase`, functions and variables `snake_case`, constants
  `SCREAMING_SNAKE`.
- Markdown wraps at 80 columns (`.editorconfig`). Prose and list items are
  rewrapped; tables, code blocks and headings are left alone.

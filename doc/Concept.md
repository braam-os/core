# Braam — Concept and Development Workbook

An interactive, CLI-oriented operating system that runs entirely inside a browser tab,
written from scratch in freestanding C++20 and compiled to WebAssembly.

This document is the project's single design reference and its working plan. It states the
goal, sets out the architecture, records the decisions we have already made and why, and
breaks the work into milestones with concrete acceptance criteria. Check the boxes as we go.

---

## 1. Goal

Build a small, self-contained operating environment — a kernel, a shell, a filesystem, a
terminal, and a set of programs — that a user reaches by opening a URL. It must be
deployable as a **static site**, with no server, no build-time secrets, and no special
HTTP headers.

That last constraint is not cosmetic; it drives the whole design:

- **No `SharedArrayBuffer`**, therefore no `COOP`/`COEP` headers, therefore GitHub Pages
  and any dumb static host will serve it as-is.
- **No Asyncify**, no JSPI, no stack-switching machinery of any kind.
- **No Emscripten runtime.** We link nothing we did not write.

### Non-goals, chosen deliberately

- **POSIX compatibility.** We are not implementing `open`/`read`/`write`/`fork`, and we are
  not aiming to run third-party C code.
- **A VT100 emulator.** No ANSI escape parsing, no `xterm.js`.
- **A general-purpose libc.** We supply exactly the foundation our own code needs.

Dropping POSIX is the single highest-leverage decision in the project. We give up the
ability to drop in existing C programs; in exchange we get a system an order of magnitude
smaller, with no emulation layers, no escape-sequence parser to be attacked through, and a
design whose every mechanism is native to the browser rather than pretending to be Unix.

---

## 2. Organizing principles

Three invariants hold the design together. Nearly every question about "how should X work?"
is answered by one of them.

### 2.1 Coroutines are processes; the event loop is the scheduler

**C++20 coroutines *are* the process abstraction, and the browser event loop *is* the
scheduler.** Every operation that would block in a conventional OS becomes a `co_await`.

Nothing ever blocks, so nothing ever needs a separate stack, and stack-switching magic
(Asyncify, JSPI, threads) is simply not part of the system. A process suspended on input is
a coroutine frame sitting in a hash map, costing one allocation.

### 2.2 An import never returns data — only a token

**Every JS import is non-blocking and returns immediately.** An import accepts a *wake
token*; the result arrives later through the `wake()` export. Uniformly. Without exception,
except where noted below.

This single rule is what keeps the boundary uniform and makes adding any new asynchronous
browser API a ~20-line change on each side. It is worth defending aggressively.

**The two conscious exceptions**, both because no promise is involved at all:

1. `host_now()` — a clock read.
2. **OPFS sync access handles** — once a file is open, `read`/`write`/`getSize`/`truncate`/
   `flush` are genuinely synchronous (see §5.2).

One or two pragmatic exceptions are fine. Three become an ad-hoc second calling convention,
and then we have two ABIs and no invariant. Each new exception needs a written justification
in this document.

### 2.3 The terminal is a cell grid, not a byte stream

The kernel owns a screen buffer of cells in linear memory. The renderer draws it. There is
no stream of bytes carrying control codes, because there are no control codes.

Consequences: colours and styling are struct fields; cursor addressing is array indexing; a
`curses`-style layout layer becomes trivial rather than a parser; and there is no escape
sequence to mis-parse. Rendering is roughly 300 lines of JavaScript.

---

## 3. Architecture

```
┌─────────────────────── main thread ───────────────────────┐
│  boot: capability probe, navigator.storage.persist()       │
│  input: KeyboardEvent → {code, mods} → postMessage         │
│  render: OffscreenCanvas (transferred to worker)           │
└───────────────────────────┬───────────────────────────────┘
                            │  postMessage (no SharedArrayBuffer)
┌───────────────────────────┴───────────────────────────────┐
│                       Web Worker                           │
│  ┌──────────────── JS host shim (~600 lines) ───────────┐ │
│  │  imports: timers, fetch, storage, present, log …     │ │
│  │  exports: init, wake, tick, key, resize              │ │
│  │  externref table · OPFS handle table · canvas blit   │ │
│  └──────────────────────┬──────────────────────────────┘ │
│  ┌──────────────────────┴─── kernel.wasm ───────────────┐ │
│  │  allocator · core types · Task<T> · scheduler        │ │
│  │  Channel<T> · Process · CancelToken                  │ │
│  │  screen cells · VFS mount table · program registry   │ │
│  └──────────────────────────────────────────────────────┘ │
│  ┌──── (M8+) per-process WebAssembly.Instance ──────────┐ │
│  │  own linear memory, own import closure, own limits   │ │
│  └──────────────────────────────────────────────────────┘ │
└────────────────────────────────────────────────────────────┘
```

The kernel runs in a **Web Worker** and communicates by plain `postMessage`. Rendering
happens against an `OffscreenCanvas` transferred into the worker, so the main thread stays
free. A runaway program hangs its own worker rather than the page, and a "reset kernel"
button is just `worker.terminate()` followed by a reboot.

### 3.1 Toolchain and language subset

Target `wasm32-unknown-unknown`, freestanding. The installed toolchain is
**`/opt/wasi-sdk-33.0`** (clang 22.1.0-wasi-sdk). We use it purely as a well-maintained
clang; we link none of its runtime and, as it turns out, none of its headers either
(see Appendix C).

```
/opt/wasi-sdk-33.0/bin/clang++ \
    --target=wasm32-unknown-unknown \
    -std=c++20 -Os \
    -nostdlib -nostdinc++ \
    -fno-exceptions -fno-rtti -fno-threadsafe-statics \
    -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined
```

This command line is verified to compile a coroutine that suspends and resumes. See
Appendix C for the one gotcha — libc++'s `<coroutine>` header cannot be used freestanding,
so we supply our own ~25-line shim over the `__builtin_coro_*` intrinsics.

**No exceptions, no RTTI.** Errors are values: `Result<T, E>`. Propagation through
`co_await` uses a small `TRY()` macro rather than unwinding.

### 3.2 The foundation we own

Roughly 1700 lines of code that we will be glad to control:

- **Allocator** — a bump arena plus size-class free lists over `memory.grow`. About 200
  lines. Coroutine frames go through it, so it must be fast (see §8.2).
- **Core types** — `Str` (a UTF-8 view), `String`, `Vec<T>`, `Span<T>`, `Result<T, E>`,
  `Option<T>`, `HashMap<K, V>`. About 1500 lines.

### 3.3 The core abstraction

```cpp
template <class T> struct Task {           // lazy, movable, awaitable
    struct promise_type { ... };           // symmetric transfer on final_suspend
};

struct Waker { u32 token; };               // handed to JS, comes back later

// Every syscall is one of these:
Task<Line>    read_line(Tty&);
Task<void>    sleep_ms(u32);
Task<Bytes>   http_get(Str url);
Task<void>    write(Stream&, Span<const u8>);
```

The scheduler is a ready queue of `std::coroutine_handle<>` plus a
`HashMap<u32, coroutine_handle<>>` of suspended tasks keyed by wake token. An awaitable's
`await_suspend` allocates a token, registers the handle, and calls a JS import that tells
the host "notify me on token N."

That is the entire kernel core: a few hundred lines.

### 3.4 The JS boundary

Deliberately tiny, one-directional per call.

**Wasm exports** (host → kernel):

```
init(heap_base)
wake(token, payload_ptr, payload_len)   // host signals an event
tick(now_ms)                            // drains ready queue; returns ms-until-next-timer, or -1
key(code, mods)                         // fast path, avoids allocation
resize(cols, rows)
```

**Wasm imports** (kernel → host), all non-blocking, all returning immediately:

```
host_now(), host_random(ptr, len), host_log(ptr, len)
host_timer(token, ms)
host_fetch(token, url_ptr, url_len, opts_ptr)
host_storage_read(token, key…), host_storage_write(token, …)
host_present(dirty_x, dirty_y, dirty_w, dirty_h)
```

### 3.5 The screen

```cpp
struct Cell { char32_t ch; u8 fg, bg, attrs; };   // 8 bytes, or pack to 8 with a palette
Cell screen[rows * cols];
```

The renderer holds a `Uint8Array` view over that region and blits monospace glyphs to the
canvas, plus a cursor. Damage tracking is a dirty rectangle the kernel updates as it writes,
passed to `host_present`.

Input is symmetric: a normalised `KeyboardEvent` becomes `{code, mods}`, is posted to the
worker, and lands in a `Channel<Key>`. Line editing — history, cursor movement, kill-word,
completion — lives in a **userland** `LineEditor` coroutine, not in the kernel. That is where
the "line discipline" belongs, and it is far nicer as a coroutine than as a termios state
machine.

### 3.6 Kernel objects

- **`Channel<T>`** — an async MPSC queue with bounded capacity: `co_await ch.recv()` and
  `co_await ch.send(v)`. This one type is our pipe, our stdin, and our IPC.
- **`Process`** — a `Task<int>` plus a name, argv, cwd, stdio channels, and a `CancelToken`.
  `spawn()` pushes it onto the scheduler. Killing means signalling the token; every
  `co_await` point checks it and unwinds by returning, so destructors run correctly.
  Structured concurrency: a parent `co_await`s a child group, and cancellation propagates
  down the tree.
- **Filesystem** — an async node tree, not inodes. One interface:

  ```cpp
  struct Fs {
      virtual Task<Result<Bytes>>       read(Path);
      virtual Task<Result<void>>        write(Path, Span<const u8>);
      virtual Task<Result<Vec<Entry>>>  list(Path);
  };
  ```

  A mount table maps prefix → `Fs`. Implementations in §5.
- **Programs** — a registry of `Task<int>(Args, Stdio)` functions, populated at static-init
  time by an inline registrar, so adding a command means adding one file and editing nothing
  else.

### 3.7 Holding JS objects

Use an **`externref` table** rather than a hand-rolled map of integer IDs. Reference types
are supported everywhere now: a `WebAssembly.Table` of `externref` that wasm indexes into and
JS populates means a `Response`, a `FileSystemFileHandle`, or a `WebSocket` is just a slot
index, with no serialisation. Wrap it in an RAII `JsRef` that frees the slot in its
destructor.

---

## 4. Process model

Isolation is **tiered by trust**, not chosen once for everything. All three tiers coexist,
and `exec` picks one from a flag in the binary's metadata — userland does not notice.

| Tier | Isolation | Spawn cost | Kill | Used for |
|---|---|---|---|---|
| **Kernel applet** | none | ~0 | cooperative | `ls`, `cd`, shell builtins — trusted code |
| **Instance, shared worker** | address space + capabilities + memory cap | ~1 ms | cooperative | normal programs |
| **Instance, own worker** | the above + liveness | ~10 ms, few MB | `worker.terminate()` | untrusted or long-running |

**Milestones M0–M7 build only the first tier.** Tiers 2 and 3 arrive in M8 and M9. The ABI
below is fixed now, as forward design, so that nothing built earlier has to be unpicked.

### 4.1 What separate instances buy

- **Address space: isolated, for free.** Two `WebAssembly.Instance`s have two separate
  `WebAssembly.Memory` objects, and there is *no* instruction that reaches outside your own
  linear memory. A wasm pointer is an offset, not an address; there is nothing to forge.
  This is a stronger guarantee than MMU-based process isolation, because it is enforced by
  the type system and bounds checks rather than by page tables you might misconfigure.
- **Capabilities: isolated, if we are careful.** An instance can only call the imports we
  supply at instantiation. Give each instance an import closure bound to its PID and we have
  a genuine capability system: process 7 physically cannot issue a syscall as process 3,
  because it holds no function that does so. The same applies to the `externref` table — hand
  each instance its own and it can only touch the objects we put in it.
- **Memory limits: isolated, and a bonus.**
  `new WebAssembly.Memory({initial: 2, maximum: 256})` is a hard 16 MB ceiling;
  `memory.grow` simply fails past it. That is an rlimit without cgroups. When a process
  exits we drop the instance and *all* its memory returns at once, sidestepping the
  fragmentation and leak problems of applets sharing the kernel heap.

### 4.2 What they do not buy: CPU time

**`while(1){}` cannot be preempted.** Nothing in the wasm specification allows it. Address-space
isolation and *liveness* isolation are separate problems, and we should be explicit about
which one we are solving. Two options:

1. **One worker per untrusted process**, so `worker.terminate()` is our `SIGKILL`. This is
   tier 3, and it is the plan.
2. **Fuel counters** — a binary-rewriting pass injecting `if (--fuel < 0) trap;` at loop
   headers and function entries. This is what standalone runtimes do for metering; it costs
   perhaps 5–15% throughput. Optional, and a self-contained project of its own.

### 4.3 The kernel↔process ABI

```
process exports:  memory, _start(argc) -> i32
                  _alloc(n) -> ptr, _free(ptr, n)
                  _resume(token, ptr, len) -> i32   // returns: 0 = done, 1 = suspended

process imports:  sys(op, a0, a1, a2) -> i32        // sync ops, immediate result
                  sys_async(op, token, ptr, len)    // async ops, reply via _resume
```

The coroutine model survives the boundary intact: the process's `co_await` suspends, its
scheduler returns control out through `_start`/`_resume`, the kernel continues, and later
calls `_resume` with the payload. Reentrant scheduling across an instance boundary, with no
stack switching.

### 4.4 Cost model

Compilation is expensive; instantiation is cheap. Call `WebAssembly.compileStreaming()` once
per binary, keep the `Module` in a cache keyed by path, and instantiate per `exec`. `Module`
objects are structured-cloneable, so we can compile once and `postMessage` the module to
every worker. Browsers also cache compiled code across page loads for streaming-compiled
modules, so `/bin` warms up after the first visit.

The real cost is **duplication**: with no dynamic linking, every binary embeds its own copy
of the allocator, the string types, and the coroutine runtime. Keep the process-side runtime
deliberately minimal and push anything substantial into syscalls, so it lives once in the
kernel rather than N times in userland.

Cross-instance data movement is covered in Appendix B.

---

## 5. Storage

Browsers do offer real persistence, and one API is a genuinely good fit. See Appendix A for
the full comparison and the durability caveats.

### 5.1 The mount layering

```
BundleFs   → Cache API blob      (read-only /bin, /usr — immutable, cheap)
OpfsFs     → OPFS                (read-write /home, /var — the real store)
MemFs      → linear memory       (/tmp, and the fallback when OPFS is absent)
HostFs     → File System Access  (/mnt/host, Chromium only, opt-in)
HttpFs     → Range requests      (read-only remote trees)
```

### 5.2 OPFS is the primary store

The Origin Private File System is a storage endpoint of the File System API, private to the
origin, invisible in the user's regular filesystem, and supported by Safari, Chrome, Edge and
Firefox. It gives real directory handles, real file handles, seekable reads and writes,
truncate, rename and remove — which maps onto our `Fs` interface almost one-to-one.

The detail that matters most for our architecture: the high-performance **synchronous**
`read()`/`write()` methods obtained via `createSyncAccessHandle()` are exposed **only inside a
Web Worker** — not the main thread, not an iframe, not even a SharedWorker. Our kernel already
lives in a worker, so we get the fast path for free.

This genuinely simplifies the design. **Opening** a file is async (one wake token), but once
we hold a sync access handle, `read`/`write`/`getSize`/`truncate`/`flush` return immediately.
Those are plain value-returning imports — the second sanctioned exception to §2.2, because no
promise is involved at any point.

Two constraints to build around:

- A sync access handle takes an **exclusive lock** on the file, so the VFS needs an open-file
  table that refuses a second writer. We want that anyway.
- OPFS is unavailable in Safari private browsing. Capability-detect and fall back to `MemFs`.

### 5.3 Capability struct, not probing

The JS host fills in a `StorageBackend` struct at boot and hands it to the kernel:

```cpp
struct StorageBackend {
    bool opfs, syncHandles, fsAccess, persisted;
    u64  quotaBytes;
};
```

`mount` consults this rather than probing at use time. Add a `df`-style command reporting
mode (persistent vs best-effort) and usage from `navigator.storage.estimate()`, so storage
semantics are inspectable from inside the OS instead of being invisible browser behaviour.

### 5.4 The real local filesystem, and the escape hatch

`showDirectoryPicker()` yields a handle to an actual folder on disk, read-write, after an
explicit user gesture and permission grant. It is the closest thing to mounting the host
filesystem, and it is how someone would edit their real project directory from our shell.

Its reach is limited (Appendix A), so treat it strictly as progressive enhancement: a `mount`
command that is simply absent when `window.showDirectoryPicker` is undefined. Directory
handles are structured-cloneable, so we can stash one in IndexedDB and re-offer the mount on
the next visit — though permission must be re-requested each session.

The universally available escape hatch is the boring one: `<input type="file">` for import
and a Blob download for export. Wire it up early as `/mnt/import` and an `export` command. It
works everywhere and covers "get my data out."

---

## 6. Milestones

Each milestone has one objective and one acceptance criterion. Tick them off as we go.

### M0 — Nucleus — **done**
Freestanding build, the coroutine shim, the allocator, `Str`/`Vec`, `host_log`.
Set a binary-size budget now and track it in CI from the first commit.

- [x] `cmake --build` produces a wasm binary with the Appendix C command line
- [x] A static page loads a 4 KB wasm and logs a line to the console
- [x] Size budget recorded (32 KiB) and enforced by CI

CMake replaces `make`. `Span<T>`, `Result<T, E>` and `Option<T>` came along with `Str`/`Vec`,
since M1 needs them immediately; `String` and `HashMap` wait for M1, where the wake-token table
will shape the latter. Appendix C's command line changed in three ways — see §C.3, and
[Release_Notes.md](Release_Notes.md) for the reasoning behind each.

### M1 — Scheduler
`Task<T>`, ready queue, wake tokens, `tick()`, `sleep_ms`.
`CancelToken` participates in every awaitable from this milestone on (§8.1).

- [ ] Two coroutines interleave sleeps in the correct order
- [ ] Cancelling a sleeping task unwinds it and runs its destructors

### M2 — Screen and keys
Cell grid, canvas renderer, damage rectangles, `Channel<Key>`, `OffscreenCanvas` transfer.

- [ ] Typed characters appear on screen and the cursor moves
- [ ] Window resize reflows and `resize(cols, rows)` reaches the kernel

### M3 — Userland shell
`LineEditor` coroutine with history and editing, tokeniser, program registry, argv, exit codes.

- [ ] `echo hello` prints, `help` lists registered programs
- [ ] Up-arrow recalls history; a nonzero exit code is observable

### M4 — Streams
`Channel<Bytes>` as stdio, pipes, redirection, cancellation on `^C`.

- [ ] `ls | grep foo` works
- [ ] `^C` interrupts a running pipeline and returns a prompt

### M5 — Filesystem
Mount table, `MemFs`, `BundleFs` from a fetched archive, `OpfsFs` with the open-file table.

- [ ] Write a file, reload the page, the file is still there
- [ ] `df` reports quota, usage, and persistent vs best-effort mode
- [ ] With OPFS unavailable, the system boots on `MemFs` and says so

### M6 — Host services
`fetch`, timers, WebSocket, clipboard, the `externref` table and `JsRef`.

- [ ] A `curl`-ish command fetches a URL and prints the body
- [ ] A chat client works over a WebSocket
- [ ] `/mnt/import` and `export` move files in and out

### M7 — Depth
A layout/widget layer over the cell grid (panes, a `less`, an editor), job control,
`/proc`-style introspection, an embedding API for host pages.

- [ ] A full-screen editor opens, edits, and saves a file
- [ ] Jobs can be backgrounded and listed

### M8 — Isolated processes
The §4.3 ABI, per-process `WebAssembly.Instance`, per-PID import closures, per-process memory
caps, module cache, cross-boundary copies (Appendix B).

- [ ] A program runs as its own instance with a 16 MB cap and `memory.grow` fails past it
- [ ] A process cannot issue a syscall on behalf of another PID
- [ ] Tier selection comes from binary metadata; userland behaviour is unchanged

### M9 — Liveness isolation
The own-worker tier: worker pool, `worker.terminate()` as `SIGKILL`, module `postMessage`.
Optionally, fuel injection as a metering alternative.

- [ ] `while(1){}` in an untrusted program is killable without reloading the page
- [ ] The shell stays responsive while such a program runs

---

## 7. Repository layout

As created in M0; the `src/fs`, `src/prog` and `src/user` directories arrive with their
milestones.

```
doc/Concept.md          this document
doc/Release_Notes.md    reasoning behind the code, milestone by milestone
CMakeLists.txt          the build
cmake/                  the wasm32-unknown-unknown toolchain file
src/kernel/             allocator, core types, Task, scheduler, Channel, Process
src/kernel/coroutine.h  the freestanding <coroutine> shim (Appendix C)
src/fs/                 Fs interface, MemFs, BundleFs, OpfsFs, HostFs, mount table
src/prog/               one file per program; self-registering
src/user/               LineEditor, shell, widget layer
test/                   in-wasm unit tests and the Node driver
web/                    index.html, worker.js, host shim, renderer
tools/                  build scripts, bundle packer, size-budget check
```

---

## 8. Things to get right early

### 8.1 Awaitables must be cancellation-aware from day one
Retrofitting cancellation into coroutine code is painful. Bake `CancelToken` into every
awaitable's `await_suspend` starting in M1, not later.

### 8.2 Coroutine frame allocation is the hot path
Frames are heap-allocated per call. A naive `malloc` will dominate the profile. Size-class
pools fix it, and the allocator should be built with this as its primary workload.

### 8.3 Never let an import return data synchronously
Beyond the two documented exceptions (§2.2). One exception is pragmatic; three are a second
ABI.

### 8.4 `memory.grow` detaches the `ArrayBuffer`
Any cached `Uint8Array` view goes dead after a growth. Re-derive views after every possible
growth, or route all access through a `view()` accessor that checks
`buffer.byteLength === 0`. This bug will bite at least once regardless; make it fail loudly.

### 8.5 Safari's 7-day eviction is a real hazard
With cross-site tracking prevention on, an origin that sees no user interaction for seven
days of browser use has all script-created data deleted. For a system whose pitch is "your
files persist," this matters. Mitigations: request persistence, encourage Add to Home Screen
(installed web apps are exempt from the ITP timer), and make `export` easy so nobody loses
irreplaceable work.

---

## Appendix A — Browser storage APIs

### A.1 The tiers

| API | Shape | Where it runs | Our use |
|---|---|---|---|
| **OPFS** | Real files and directories, origin-private, invisible to the user | Async API anywhere; **sync** handles worker-only | **Primary store** |
| **IndexedDB** | Async key → blob, transactional | Anywhere | Fallback; metadata; stashed directory handles |
| **Cache API** | Request → Response pairs | Anywhere | The fetched bundle blob |
| **localStorage** | 5 MB, sync, strings only | Main thread only | Tiny config, nothing else |
| **File System Access** | The *actual* user disk, with a picker | Chromium desktop only | Optional `/mnt/host` |

### A.2 Durability

Storage is **best-effort by default**, meaning it can be deleted without asking. Three facts:

- An origin can opt into persistent mode via `navigator.storage.persist()`, after which data
  is evicted only if the user chooses to delete it. **`persist()` is not available in Web
  Workers** — call it from the main thread during boot and pass the result down to the kernel
  in `StorageBackend`.
- Quotas are generous but finite. In Firefox, best-effort mode gives an origin the smaller of
  10% of disk or a 10 GiB per-site-group limit; origins granted persistent storage may use up
  to 50% of disk, capped at 8 TiB and exempt from the group limit. Safari's overall quota for
  a browser app is up to 80% of total disk space. Surface `navigator.storage.estimate()` as
  `df` — it is a nice touch and it makes the limits visible.
- **Eviction is all-or-nothing per origin.** If it fires, OPFS *and* IndexedDB *and* Cache go
  together. There is therefore no point using one as a backup of another.

### A.3 File System Access reach

Firefox and Safari ship only OPFS, and no mobile browser exposes the pickers. Safari supports
none of `showOpenFilePicker`, `showSaveFilePicker`, or `showDirectoryPicker` on macOS, iPadOS,
or iOS. Hence: progressive enhancement only, never a dependency.

---

## Appendix B — Cross-instance data movement

Instances cannot call each other, so every transfer is a copy through the host.

**The straightforward version is JS:**

```js
const src = new Uint8Array(proc.memory.buffer, ptr, len);
kernelU8.set(src, dstPtr);
```

That is memcpy speed plus one call boundary — perfectly fine for syscall-sized payloads, and
where we should start.

**If we want the kernel itself to do the copy, multi-memory is the tool.** A module may
declare several memories, and `memory.copy` can move bytes between two of them. It is at
phase 5 and was live in browsers other than Safari as of early 2025, with a Safari
implementation ticket assigned — check `wasm-feature-detect` rather than trusting that.

The wrinkle: imports are fixed at instantiation, so the kernel cannot dynamically import a
new process's memory. The trick is a tiny per-process **bridge module** that imports both the
kernel memory and that process's memory and exports `copy_in`/`copy_out`. It is about 30
bytes of wasm, instantiated alongside each process.

And note §8.4 again here: `memory.grow` detaches the `ArrayBuffer`, killing every cached view
on both sides of the copy.

---

## Appendix C — Verified toolchain notes

Verified against the installed `/opt/wasi-sdk-33.0` (clang 22.1.0-wasi-sdk, default target
`wasm32-unknown-wasip1`, sysroot supplied by `bin/clang++.cfg`).

### C.1 libc++'s `<coroutine>` cannot be used freestanding

It is often said that `<coroutine>` is header-only and compiler-intrinsic, so it works
freestanding as soon as `operator new` exists. That is true of the *language feature* but not
of this SDK's header. Compiling `#include <coroutine>` with
`--target=wasm32-unknown-unknown -nostdlib` fails, because libc++'s `<coroutine>` includes
`__functional/hash.h` → `<cstring>` → `<cmath>`, which need libc declarations (`size_t`,
`memcpy`, `FP_NAN`, …) that the bare `wasm32-unknown-unknown` target has no sysroot for.

Note also that this SDK stores libc++ headers **per target** under
`share/wasi-sysroot/include/<triple>/{eh,noeh}/c++/v1`, and there is no `unknown-unknown`
variant. The generic `include/c++/v1` directory exists but is empty.

### C.2 What does work

A hand-written shim of about 25 lines over the `__builtin_coro_*` intrinsics — declaring
`std::coroutine_traits`, `std::coroutine_handle<>`, `std::coroutine_handle<P>`,
`std::suspend_always` and `std::suspend_never` — compiles cleanly with:

```
--target=wasm32-unknown-unknown -std=c++20 -Os \
-nostdlib -nostdinc++ -fno-exceptions -fno-rtti -fno-threadsafe-statics
```

Verified with a coroutine that performs `co_await` and `co_return`. This is consistent with
the project's premise of owning its own foundation: the shim is a deliberate part of M0, not
a workaround. It lives in `src/kernel/coroutine.h`.

The intrinsics the shim needs: `__builtin_coro_resume`, `__builtin_coro_destroy`,
`__builtin_coro_done`, `__builtin_coro_promise`, and `__builtin_coro_noop` for
`noop_coroutine()`.

Note that `std::coroutine_traits` must be *defined*, not merely declared: a forward declaration
compiles until the first coroutine, which then fails to instantiate it.

### C.3 Amended in M0

Building the nucleus corrected three flags. The command line as used is:

```
/opt/wasi-sdk-33.0/bin/clang++ \
    --no-default-config \
    --target=wasm32-unknown-unknown \
    -std=gnu++20 -Os \
    -nostdlib -nostdinc++ \
    -fno-exceptions -fno-rtti -fno-threadsafe-statics \
    -ffunction-sections -fdata-sections \
    -Wl,--no-entry -Wl,--gc-sections \
    -Wl,--stack-first -Wl,-z,stack-size=131072
```

- **`--export-dynamic` is gone.** It is not a reliable way to export: it dropped a plain
  `extern "C"` function while exporting `operator new`. Exports are named individually with
  `__attribute__((export_name(...), used))`.
- **`--allow-undefined` is gone.** Imports carry `__attribute__((import_module("host"),
  import_name(...)))`, so nothing is left to resolve — and without the flag, an accidental libc
  dependency is a link error instead of a runtime trap. `memcpy`/`memset` do not leak in:
  bulk-memory is on by default and LLVM lowers them inline.
- **`--no-default-config` and `--stack-first` are new.** The first suppresses the SDK's config
  file, which injects a wasi sysroot. The second puts the shadow stack below the data segment,
  so overflow traps rather than corrupting globals (§8.4).
- `gnu++20` rather than `c++20`, because `TRY()` is a statement expression.

Full reasoning in [Release_Notes.md](Release_Notes.md).

---

## Appendix D — Provenance

This document consolidates three earlier design notes — on the nucleus, on browser storage,
and on wasm isolation — which have been removed now that their content lives here in full. It
resolves the one disagreement between them (in-kernel coroutine programs versus separate
instances) in favour of the tiered model in §4, with the isolated tiers deferred to M8 and M9
and the ABI fixed up front.

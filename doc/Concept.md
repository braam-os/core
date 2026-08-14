# Braam — Concept

An interactive, CLI-oriented operating system that runs entirely inside a browser tab,
written from scratch in freestanding C++20 and compiled to WebAssembly.

This document is the project's single design reference. It states the goal, sets out the
architecture, and records the decisions we have already made and why. It changes when a
decision changes, and then in the same commit as the code. The working plan —
milestones M0–M9 with their acceptance criteria — is in [Milestones.md](Milestones.md).

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

M6 added `fetch`, WebSocket, the clipboard, file transfer and a wall clock, and asked for no
third exception: every one of them is a promise on the host side, so every one of them takes
a token. The wall clock is the near miss — `Date.now()` is as synchronous as `host_now()` —
but a service already had an import and one more operation on it costs nothing, while a
second value-returning import costs the invariant.

There is one call in the other direction that carries no token, and it is not an exception to
this rule because it is an *export*: `ref(slot, obj)` (§3.7) is how the host puts a JS object
into the kernel's table. It stores and returns; nothing is scheduled by it.

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
└───────────────────────────┬────────────────────────────────┘
                            │  postMessage: bind, step
┌───────────────────────────┴───────────────────────────────┐
│         (M9+) Web Worker, one untrusted process            │
│  the same instance, one thread further out, where          │
│  terminate() does not need its cooperation                 │
└────────────────────────────────────────────────────────────┘
```

The kernel runs in a **Web Worker** and communicates by plain `postMessage`. Rendering
happens against an `OffscreenCanvas` transferred into the worker, so the main thread stays
free. A runaway program hangs its own worker rather than the page, and a "reset kernel"
button is just `worker.terminate()` followed by a reboot.

Since M9 a runaway program does not even hang the kernel's worker: a tier-3 process is a worker
of its own (§4.2), and the kernel is merely waiting for a reply it can stop waiting for.

### 3.1 Toolchain and language subset

Target `wasm32-unknown-unknown`, freestanding. Any clang with the wasm32 target and `wasm-ld`
will do, because we use it purely as a compiler: we link none of its runtime and, as it turns
out, none of its headers either (see Appendix C). Homebrew's **`llvm`** and **`lld`** are the
local default; CI pins **`/opt/wasi-sdk-33.0`** (clang 22.1.0-wasi-sdk), which is what
Appendix C was verified against.

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
resize(cols, rows)                      // returns the screen descriptor's address, or 0
ref(slot, obj)                          // host deposits a JS object in the table (§3.7)
sys(pid, op, a0, a1, a2) -> i32         // a process's synchronous syscall (§4.3)
sys_async(pid, op, token, len) -> i32   // a process's asynchronous syscall (§4.3)
```

The last two arrived with M8 and are the only exports that are not the host's own business:
they are an isolated process's two imports, which the host forwards with the pid it bound into
that process's closure. A process therefore cannot name another — there is no argument for it
on its side of the call. Both are entered from JS at top level, never from inside a kernel
import, which is what keeps them as ordinary as `key()`.

`resize` returns where the screen descriptor (§3.5) lives, which is how the host learns the
geometry and the address of the cell array. It is the only call that moves the cells, so it is
also where the renderer re-derives its views (§8.4). The kernel clamps the geometry it is
given, so the host reads `cols` and `rows` back out of the descriptor rather than assuming its
request was honoured.

**Wasm imports** (kernel → host), all non-blocking, all returning immediately:

```
host_now(), host_random(ptr, len), host_log(ptr, len)
host_present(dirty_x, dirty_y, dirty_w, dirty_h)
host_fs(op, token, req)                       // storage, async  (§5.2)
host_fs_sync(op, handle, ptr, len, off) -> i32 // storage, sync   (§5.2)
host_svc(op, token, req, ref)                 // host services, async (§3.7)
```

`host_fetch` above was M6's, and it is not what M6 built: naming an import per operation is
the style M5 replaced. `host_svc` carries fetch, WebSocket, the clipboard, file transfer and
the wall clock over the same `req` record `host_fs` uses, with the object the operation acts
on passed alongside as an `externref`.

There is no `host_timer`. The kernel owns the timer queue, so `tick()`'s return value already
says when the host must call back, and one `setTimeout` serves every sleeping task.

Storage is **multiplexed rather than named per operation**, which is what `host_storage_read`
and `host_storage_write` above became in M5. One import per *calling convention* — one
asynchronous, one synchronous — is the shape §4.3 already fixes for the process ABI, and it
keeps the exact-import assertion in the smoke test stable while operations are added. `req` is
the address of a request record carrying the string argument, the flags, a reply buffer and
the status; the kernel owns that record for as long as the host may touch it, which is past a
cancelled await, so it outlives its awaiter rather than being freed under the host.

M6 generalised that record rather than writing a second one: it is `HostRequest` in
`src/kernel/hostcall.h`, and the interface a call belongs to picks the import. Both
asynchronous imports therefore have one wire format, one orphan list and one reaper.

M8 added **no import at all**. Compiling a binary, instantiating it and stepping it are
asynchronous operations on the host, which is `host_svc`'s convention exactly, so they are three
more of its operations rather than an interface of their own. The record gained one word, `aux`,
because those three need to name a process and `op` and `flags` were spoken for.

### 3.5 The screen

```cpp
struct Cell { char32_t ch; u8 fg, bg, attrs, reserved; };   // 8 bytes; fg and bg are palette indices
Cell screen[rows * cols];
```

The renderer holds a view over that region and blits monospace glyphs to the canvas, plus a
cursor. Damage tracking is a dirty rectangle the kernel updates as it writes, passed to
`host_present` once per `tick`. The cursor is drawn, never stored, so moving it dirties the
cell it left as well as the one it entered.

The host finds all of this through a descriptor, whose address `resize` returns:

```cpp
struct Screen {
    u32 magic;                 // 'BSCR', so a mismatched renderer fails loudly (§8.4)
    u32 cols, rows;
    u32 cursor_x, cursor_y;    // cursor_x may equal cols: the wrap is deferred
    u32 cursor_on;
    u32 cells;                 // address of Cell[cols * rows]
};
```

The wrap is deferred, so filling the last column does not scroll the screen on its own. A
resize keeps the rows in use — `0..cursor_y` — dropping from the top when they no longer fit,
and lands them at the top of the new grid. Re-wrapping logical lines needs a line model the
grid does not have; M7 built the layout layer and left the promise unkept, because the model
belongs *in* the grid rather than above it — a per-row continuation bit written by
`screen_put` — and it lands with whichever milestone needs scrollback.

The layout layer over the grid arrived with M7, in `src/ui/`, and it is three small things
rather than a widget toolkit:

- **`Pane`** — a rectangle with its own coordinates, style and cursor. Every write is clipped
  to it, so a status line cannot scribble on the text above it. A pane writes cells directly
  and marks them with `screen_touch`; it never scrolls, because scrolling moves the whole grid.
- **`FullScreen`** — the alternate screen as RAII. The grid is copied to a heap block, blanked,
  and copied back by the destructor, which is what gives the shell's screen back when a program
  is cancelled while suspended.
- **`TextBuf` and `TextView`** — logical lines and a window onto them. `less` and `edit` differ
  in what they do with keys, not in how they scroll.

Input is symmetric: a normalised `KeyboardEvent` becomes `{code, mods}`, is posted to the
worker, and lands in a `Channel<Key>`. A printable key carries its Unicode codepoint; named
keys take values above the Unicode range. **No control characters exist anywhere in the
system**: `^C` is `'c'` with the control modifier set, and the reader decides what that means.
That is §2.3 applied to input. Line editing — history, cursor movement, kill-word, completion —
lives in a **userland** `LineEditor` coroutine, not in the kernel. That is where the "line
discipline" belongs, and it is far nicer as a coroutine than as a termios state machine.

There is exactly one receiver on that channel, and while a pipeline runs it is the tty pump. A
full-screen program therefore **does not take the keyboard — it claims a route through the
pump**: `KeyInput` for raw keys with no echo, `InputClaim` to send the cooked bytes to another
job's stdin, which is what `fg` needs. `^C` is never routed; it cancels the pipeline whatever
is claimed, so a program that has taken the screen and stopped answering stays killable.

### 3.6 Kernel objects

- **`Channel<T>`** — an async MPSC queue with bounded capacity: `co_await ch.recv()` and
  `co_await ch.send(v)`. This one type is our pipe, our stdin, and our IPC.
- **`Process`** — a `Task<int>` plus a name, argv, cwd, stdio channels, and a `CancelToken`.
  `spawn()` pushes it onto the scheduler. Killing means signalling the token; every
  `co_await` point checks it and unwinds by returning, so destructors run correctly.
  Structured concurrency: a parent `co_await`s a child group, and cancellation propagates
  down the tree. The name is a view the scheduler keeps, and `sched_procs()` reads the table
  back out — which is what /proc is made of (§5.1).
- **The job table** — a pipeline started with `&` outlives the shell frame that started it, so
  M7 filed it here: an id, the command text, the stages' pids, and a reaper task standing where
  the shell stands for a foreground pipeline. `jobs`, `fg` and `kill` are ordinary programs
  over it, and the shell announces a finished job at the next prompt. There is no `bg` and no
  `^Z`: stopping a running coroutine at an arbitrary point is the resume-side twin of
  `CancelToken` and would have to reach every awaitable.
- **Filesystem** — an async node tree, not inodes. One interface, split by *when* the work can
  happen rather than by what it does: naming a file may need the host and therefore a wake
  token, but an already-open file does not (§5.2).

  ```cpp
  struct Fs {
      virtual Task<Result<Stat>>       stat(Str path);
      virtual Task<Result<Vec<Entry>>> list(Str path);
      virtual Task<Result<u32>>        open(Str path, u32 flags);
      virtual Task<Result<void>>       mkdir(Str path);
      virtual Task<Result<void>>       remove(Str path, bool all);

      virtual Result<usize> read(u32 h, u64 off, u8 *buf, usize n);
      virtual Result<usize> write(u32 h, u64 off, const u8 *buf, usize n);
      virtual Result<u64>   size(u32 h);
      virtual Result<void>  truncate(u32 h, u64 n);
      virtual void          close(u32 h);
  };
  ```

  `read` returns bytes into a caller's buffer rather than a `Bytes`, for the reason a pipe
  carries a `String`: a `Span` is a pointer and a length, and nothing below this line owns a
  buffer the caller can keep. An implementation sees paths already resolved and relative to its
  own mount point, so it never has to know where it was mounted.

  A mount table maps prefix → `Fs`, longest prefix winning, and an open-file table above it
  holds the descriptors. Implementations in §5.
- **Programs** — a registry of `Task<int>(Args, Stdio)` functions, populated at static-init
  time by an inline registrar, so adding a command means adding one file and editing nothing
  else.

### 3.7 Holding JS objects

Use an **`externref` table** rather than a hand-rolled map of integer IDs. Reference types
are supported everywhere now: a table of `externref` that wasm indexes into means a
`Response`, a `FileSystemFileHandle`, or a `WebSocket` is just a slot index, with no
serialisation. Wrap it in an RAII `JsRef` that frees the slot in its destructor.

M5's OPFS handles are a plain JS array indexed by slot number, not this. The table arrived
with M6, where `fetch` and `WebSocket` make more than one kind of object need holding; a
filesystem handle is already only ever an integer on the wasm side, so it gained nothing from
going first.

**The table is the kernel's, and the host never indexes it.** That is the one detail this
section had backwards. `import_module`/`import_name` apply to functions only, so a table
cannot be imported from JS; a module-defined one is what the toolchain supports. So the
traffic runs the other way round:

- The kernel reserves a slot and publishes the number in the request record.
- The host, when its promise resolves, calls the `ref(slot, obj)` export to deposit the object.
- To *use* it, the kernel reads the slot and passes the object as `host_svc`'s fourth
  argument. JS sees the object, never the table.

That also makes M8's capability story simpler than the original sketch: an instance's table is
part of the instance, so an isolated process can only reach the objects its own kernel put
there, with no per-instance table to hand out.

A slot is owned. `JsRef` is move-only and clears its slot in its destructor; a request that
reserved one owns it until `await_resume` hands it over, so a cancelled request frees the slot
along with the record. Releasing a *service* object additionally tells the host to let go —
a socket has event handlers holding it alive on the JS side — which is what `JsHandle` in
`src/svc/svc.h` adds on top.

---

## 4. Process model

Isolation is **tiered by trust**, not chosen once for everything. All three tiers coexist,
and `exec` picks one from a flag in the binary's metadata — userland does not notice.

| Tier | Isolation | Spawn cost | Kill | Used for |
|---|---|---|---|---|
| **Kernel applet** | none | ~0 | cooperative | `ls`, `cd`, shell builtins — trusted code |
| **Instance, shared worker** | address space + capabilities + memory cap | ~1 ms | cooperative | normal programs |
| **Instance, own worker** | the above + liveness | ~10 ms, few MB | `worker.terminate()` | untrusted or long-running |

**M0–M7 built only the first tier; M8 built the second and M9 the third.** `exec` reads the tier
out of a binary's `braam` custom section (§4.3): a name that is in the program registry is an
applet, a name in `/usr/bin` is a binary, and the binary says which of the other two it wants.
A binary asking for tier 3 still runs at tier 2 where the host has no worker to put it in —
which is now a *fallback* rather than a milestone, and covers a browser without nested workers
and a `procworker.js` that will not load.

One thing the tier does decide, and it is not userland's business either: the in-wasm unit tests
can drive an applet and cannot drive a binary, because stepping an instance means returning to
the host and `run_tests()` never does. Whatever `test/unit/` runs has to stay an applet.

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
   tier 3, and it is what M9 built.
2. **Fuel counters** — a binary-rewriting pass injecting `if (--fuel < 0) trap;` at loop
   headers and function entries. This is what standalone runtimes do for metering; it costs
   perhaps 5–15% throughput. Optional, and a self-contained project of its own.

The first is enough for a *kill*, which is what an operating system owes its user, and it needs
no metering: the kernel does not have to notice that a process is looping, because it is not
waiting on anything it cannot abandon. A tier-3 step is one more asynchronous host request, so a
process that never answers is a request that never lands — and `^C`, `kill` and a cancelled job
already know what to do with one of those. The second option is still the only way to *bound*
CPU rather than end it, and is still unbuilt.

What tier 3 does not change is the shape: one worker per process, hired from a small pool and
terminated rather than pooled when it is killed, and the process's own memory created inside it,
so the kernel's page counts still decide the cap. A worker that has finished its process is
clean — the instance is dropped and wasm cannot have touched the worker's own scope — so it goes
back to the pool. One that was terminated is gone, which is the point.

### 4.3 The kernel↔process ABI

As built in M8. The shape is the one this section fixed in M0; four details are amended below,
and each is marked.

```
process imports:  env.memory                        // the kernel's, so the cap is the kernel's
                  sys(op, a0, a1, a2) -> i32        // sync ops, immediate result
                  sys_async(op, token, ptr, len)    // async ops, reply via _resume

process exports:  _start(argv_ptr, argv_len) -> i32 // 0 = exited, 1 = suspended
                  _resume(token, ptr, len)   -> i32 // the same
                  _alloc(n) -> ptr, _free(ptr, n)

custom section "braam":  magic, abi, tier, flags, initial_pages, max_pages
```

The coroutine model survives the boundary intact: the process's `co_await` suspends, its
scheduler returns control out through `_start`/`_resume`, the kernel continues, and later
calls `_resume` with the payload. Reentrant scheduling across an instance boundary, with no
stack switching.

**Memory is imported rather than exported.** `--import-memory` and no declared maximum means
the host supplies `new WebAssembly.Memory({initial, maximum})`, so the 16 MB ceiling of §4.1 is
the kernel's decision and not a number the binary could have written differently.

**`_start` takes argv rather than argc.** The host has to place the argv blob in the process's
memory — through `_alloc`, which is what `_alloc` is for — and `argc` alone cannot say where it
put it. The blob is `u32 argc`, then a length and bytes per word.

**A reply payload begins with an `i32` status.** `_resume`'s signature has room for a buffer and
not for an errno, and every asynchronous syscall needs both.

**The kernel does not call a process; the host does, and never with the kernel on the stack.**
Only JS can call another instance's exports, and re-entering the kernel from inside one of its
own imports would run it on a heap it is halfway through changing. So one `_start` or `_resume`
is a *deferred host action*, structurally identical to a storage reply: the process's proxy task
in the kernel parks on a wake token, the host steps the instance once the tick has unwound, and
the token is woken with the outcome. Synchronous syscalls run the other way and need no such
care — they re-enter the kernel at top level, exactly as `key()` and `wake()` do.

What crosses is bytes, not addresses (Appendix B). The host asks the kernel for room with
`Sys::Stage`, copies the payload in, and only then reports the request; the reply travels back
through a block the host takes from the process's own `_alloc`.

A trap is how a process reports a fatal error: it has no host imports to log through, so the
kernel turns a trap into an exit status and says the process crashed.

**Tier 3 changes none of it.** The same binary runs at either tier; only the wiring behind its
two imports differs, which is what lets `exec` pick a tier from metadata without userland — or
the program — noticing. That is worth stating plainly, because a worker boundary has no
synchronous direction at all (§1 rules out `SharedArrayBuffer`, and therefore `Atomics.wait`),
and `sys` is by construction synchronous. The reason it survives is that every one of its four
operations can be answered *without the kernel*:

- `GetPid` is the pid the host bound into the worker when it made it — the same closure trick as
  at tier 2, one thread further out, so a process still holds no function that names another.
- `Now` is a clock reading the step message carried, plus the worker's own elapsed time. It is
  monotonic and relative rather than bit-identical to the kernel's tick clock, which nothing in
  `src/proc/` or `src/bin/` depends on.
- `Exit` is buffered and rides back on the step's reply. A process only ever issues it
  immediately before returning, so nothing observes the delay.
- `Stage` is refused with 0, the "no room" answer the runtime already handles. It is the
  *host's* syscall rather than a program's — but a hostile binary can still call it, so it needs
  an answer rather than an assumption. Any unknown operation is likewise refused locally.

So the asynchronous half is the only thing that crosses: `sys_async` is recorded beside the step
result, and the kernel worker performs the `Sys::Stage` copy on the process's behalf exactly as
the tier-2 closure does. One message down, one up, per step — the protocol between the two
workers is the *host's*, not an ABI a binary can see, and it is written once in `web/proc.js`
with both halves in the same file.

Two things do lose fidelity, and neither is worth an ABI change. A tier-3 instance is created
inside its worker, so a binary that will not instantiate reads as a crash (132) rather than as
"will not instantiate" (126) — the module is still compiled in the kernel worker, so a malformed
one is still refused before anything runs. And `Now`, as above, is relative.

### 4.4 Cost model

Compilation is expensive; instantiation is cheap. Keep the `Module` in a cache keyed by path and
instantiate per `exec`, which is what `web/proc.js` does. `Module` objects are
structured-cloneable, so we can compile once and `postMessage` the module to every worker.

The compile is *not* streaming, as this section assumed it would be: a binary reaches the host
as bytes the kernel read through the VFS, so it can come from OPFS or a copy in `/home` and not
only from a URL beside `kernel.wasm`. `new WebAssembly.Module(bytes)` is synchronous, which is
allowed in a worker at any size and keeps `exec` one round trip rather than two.

The `postMessage` of a module is what M9 uses, and it is why the cache stays in the kernel worker
rather than moving out with the instance: a binary is compiled once however many workers run it.
Starting a worker is the other cost the tier adds, and the pool is the answer — a small free list
of workers with no process in them, topped up with one at boot, which doubles as the capability
probe. Where the constructor throws, tier 3 is off and §4's fallback applies.

A tier-3 **syscall** is the cost that does not go away: two `postMessage` hops and two copies,
order 0.1 ms, against a direct call and one copy at tier 2. That is the reason the tier is a
claim a binary makes rather than a default — a syscall-bound program pays it per `SYS_CHUNK`.

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
BundleFs   → one packed archive  (read-only /usr — immutable, cheap)
BinFs      → the program registry (read-only /bin)
OpfsFs     → OPFS                (read-write /home, /var — the real store)
MemFs      → linear memory       (/, /tmp, and the fallback when OPFS is absent)
HostFs     → File System Access  (/mnt/host, Chromium only, opt-in)
HttpFs     → Range requests      (read-only remote trees)
```

Two of those differ from what this section first said, and both for the same reason — the thing
they were to hold does not exist yet:

- **`BundleFs` reads one archive, not the Cache API.** The Cache API stores `Request`/`Response`
  pairs, which is worth having once `fetch` exists to produce them; that is M6. Until then the
  worker loads a single `bundle.bin` beside `kernel.wasm` at boot and hands the bytes over, and
  the tree is unpacked in memory. The packer is `tools/pack.py`. M6 built the `fetch` and left
  the archive alone: a tree that never changes after the build has nothing to gain from a
  cache with an eviction policy.
- **`/mnt/import` is a directory, not a mount.** The picker hands over bytes, and bytes are not
  a filesystem; `import` writes them into the root `MemFs` and everything above works as it
  would for any other file. A read-through `Fs` over `File` objects would be the richer design
  and buys nothing §5.4 asks for.
- **`/bin` is `BinFs`, a filesystem over the program registry.** Programs are in-kernel
  coroutines until M8 gives them binaries, so `/bin` would otherwise be an empty directory that
  `ls` could not account for. A file there reads as the program's usage line.
- **`/proc` is `ProcFs`, the same trick over the scheduler** (M7): `meminfo`, `uptime`,
  `version`, `mounts`, `jobs`, and one file per live pid. `cat` and `grep` are then the
  introspection tools and there is no second interface to keep in step. The tree is flat —
  `/proc/42` is a file, not a directory — because a process here has one line of state, and a
  generated directory level would hold exactly one file. Content is produced at `open` and read
  out of that snapshot, so a two-block read cannot describe two different moments.

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
  table. It refuses a *second open of any kind*, not merely a second writer: OPFS's lock does
  not care what mode the second handle asks for, and a rule that held only on some backends
  would be worse than the restriction.
- OPFS is unavailable in Safari private browsing. Capability-detect and fall back to `MemFs`.

### 5.3 Capability struct, not probing

The kernel asks once, at boot, and keeps the answer:

```cpp
struct StorageBackend {
    bool opfs, sync, fsaccess, persisted;
    u64  quota, usage;
};
```

`mount` consults this rather than probing at use time. It arrives as the reply to one `Info`
operation rather than being pushed into the kernel by a separate export, which keeps the
boundary to the two imports of §3.4 and means `df` can ask again for a fresh `usage` instead of
reporting a boot-time snapshot.

`persisted` is the one field the worker cannot obtain: `navigator.storage.persist()` exists only
on the main thread (§A.2). The page calls it during boot and posts the answer down, and the
worker's boot waits for it — reporting the wrong durability is worse than a tick of delay. It is
a *bounded* wait since M7, because the call is not always a tick: Firefox took over five seconds
to answer it, which is a blank screen rather than a delay. The page sends a provisional
best-effort answer if the browser has not decided within a grace period, and the real answer
after it; the second one corrects the store, so `df` is right from then on. The request is made
once per page however many terminals are mounted, since persistence belongs to the origin.

`df` reports the backend, the mode (persistent vs best-effort), the quota and the usage, so
storage semantics are inspectable from inside the OS instead of being invisible browser
behaviour.

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

Both landed in M6, and both live on the **page** rather than in the worker: a file picker and
a download need the DOM, as does `navigator.clipboard`. `web/svc.js` relays those three across
`postMessage` and answers by id, which is invisible from the kernel — a service operation is a
token either way. The picker opens inside the transient activation of the keystroke that ran
the command, which is why `import` works without a button of its own.

**Reading the clipboard does not fit that pattern, and cannot.** `navigator.clipboard.readText()`
is only permitted from inside a user-gesture handler, and a command's request reaches the page
after the keystroke's handler has returned — so the call is never in one. Safari refuses,
Firefox does not offer it to page content, and Chrome prompts. The way out is that a **paste is
itself the gesture**: the `paste` event hands the text to the page with no permission at all, in
every browser. So a refused read becomes a wait for one, and `pbpaste` says so rather than
failing. That is why `Ctrl+V` is on the reserved list in `web/keys.js` — the kernel must not eat
the keystroke that produces the event.

---

## 6. Milestones

Moved to **[Milestones.md](Milestones.md)**: M0–M9, each with one objective and one acceptance
criterion, checked off as work lands. They live apart from this document because they change on
ordinary work commits and the design does not. This section keeps its number, since the
numbering here is cited from source comments.

---

## 7. Repository layout

As created in M0; `src/prog` and `src/user` arrived with M3, `src/fs` with M5, `src/svc`
with M6, `src/ui` with M7, and `src/proc` and `src/bin` with M8. `braam_ui` is a sibling of
`braam_fs` and `braam_svc`: above the kernel, below userland, and depending on neither of the
other two. `src/proc` is in none of that hierarchy — it is a *different binary's* runtime, and
the only thing it shares with the kernel is a handful of headers and the allocator.

```
doc/Concept.md          this document
doc/Milestones.md       the plan: M0–M9 and their acceptance criteria
doc/Release_Notes.md    reasoning behind the code, milestone by milestone
Makefile                wrapper: all, run, serve, clean
CMakeLists.txt          the build
cmake/                  the wasm32-unknown-unknown toolchain file
src/kernel/             allocator, core types, Task, scheduler, Channel, Process
src/kernel/coroutine.h  the freestanding <coroutine> shim (Appendix C)
src/kernel/hostcall.h   the asynchronous host request, shared by both interfaces
src/kernel/jsref.h      the externref table and JsRef (§3.7)
src/kernel/sysabi.h     the kernel↔process wire, included by both sides (§4.3)
src/proc/               a process binary's whole runtime: _start, syscalls, stdio
src/bin/                one file per program that is a binary of its own
src/fs/                 Fs interface, path, VFS, MemFs, BundleFs, OpfsFs, storage ABI
src/svc/                fetch, WebSocket, clipboard, file transfer, wall clock
src/ui/                 the layout layer: Pane, FullScreen, TextBuf, TextView (§3.5)
src/prog/               one file per program; self-registering
src/user/               LineEditor, grammar, job runtime, shell, exec, BinFs, ProcFs, boot
bundle/                 the tree tools/pack.py packs into /usr
test/                   in-wasm unit tests, the Node driver, the storage and service fakes
web/                    braam.js (the embedding API), worker.js, host shim, shims, renderer
tools/                  build scripts, bundle packer, size-budget check, chat server
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
where we should start. It is where M8 did start, and it is still there: two lines inside the
per-pid `sys_async` closure in `web/proc.js`.

The only wrinkle it needed was `dstPtr`. The kernel cannot be handed a buffer it did not
allocate, so the host asks for one: `Sys::Stage` is a synchronous syscall the *host* issues on
the process's behalf, returning the address of a staging block the process's kernel-side record
owns. The reverse direction needs no such call, because `_alloc` is already in the ABI.

**M9 added a third route, for the case where the two memories are not in the same agent.** A
tier-3 process is a worker away, so the copy is in two halves with a `postMessage` between them:
the process's worker `slice`s the payload out into a transferable `ArrayBuffer`, and the kernel's
worker copies that into the staging block `Sys::Stage` gave it. The kernel half of that is the
same two lines as above; only the source changed. `slice` rather than `subarray` is load-bearing
on both sides — a view is detached by the next `memory.grow` (§8.4), and one that has been
transferred cannot be re-derived.

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

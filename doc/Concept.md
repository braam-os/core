# Braam — Concept

An interactive, CLI-oriented operating system that runs entirely inside a browser tab, written
from scratch in freestanding C++20 and compiled to WebAssembly.

This document is the specification: what the system is and what its parts must do. It changes
only when a design decision changes, in the same commit as the code. Its section numbers are
cited from source comments — amend a section, do not renumber it.

The other three documents are subordinate to this one.
[Release_Notes.md](Release_Notes.md) says *why* the code is the way it is, and holds the
milestones M0–M9 with the criteria they were accepted against.
[System_Calls.md](System_Calls.md) walks §4.3's kernel↔process mechanism end to end, with the
operation table in full. [Programming_Manual.md](Programming_Manual.md) is the SDK's guide.

---

## 1. Goal

A kernel, a shell, a filesystem, a terminal and a set of programs, reached by opening a URL and
deployable as a **static site** — no server, no build-time secrets, no special HTTP headers.

That constraint drives the design:

- **No `SharedArrayBuffer`**, therefore no `COOP`/`COEP` headers, therefore any dumb static
  host serves it as-is.
- **No Asyncify**, no JSPI, no stack-switching machinery of any kind.
- **No Emscripten runtime.** Nothing is linked that we did not write.

Non-goals, chosen deliberately:

- **POSIX compatibility.** No `open`/`read`/`write`/`fork`, and no aim to run third-party C.
- **A VT100 emulator.** No ANSI escape parsing, no `xterm.js`.
- **A general-purpose libc.** Exactly the foundation our own code needs.

Dropping POSIX is the highest-leverage decision in the project: it costs the ability to drop in
existing C programs and buys a system an order of magnitude smaller, with no emulation layer, no
escape-sequence parser to be attacked through, and every mechanism native to the browser.

---

## 2. Organizing principles

Three invariants hold the design together, and nearly every "how should X work?" is answered by
one of them.

### 2.1 Coroutines are processes; the event loop is the scheduler

C++20 coroutines *are* the process abstraction and the browser event loop *is* the scheduler.
Everything that would block becomes a `co_await`. Nothing blocks, so nothing needs a stack of
its own: a suspended process is a coroutine frame in a hash map, costing one allocation.

### 2.2 An import never returns data — only accepts a token

Every JS import is non-blocking and returns immediately. It accepts a *wake token*; the result
arrives later through the `wake()` export. This keeps the boundary uniform and makes any new
asynchronous browser API a ~20-line change on each side.

**Two exceptions are sanctioned**, both because no promise is involved at any point:

1. `host_now()` — a clock read.
2. **OPFS sync access handles** — once a file is open, `read`/`write`/`getSize`/`truncate`/
   `flush` are genuinely synchronous (§5.2).

A third needs a written justification in this document. One or two pragmatic exceptions are
fine; three are a second calling convention, and then there are two ABIs and no invariant.

Calls in the other direction are not exceptions to this rule, because they are *exports*:
`ref(slot, obj)` (§3.7) stores a JS object and returns, and `sys`/`sys_async` (§4.3) are a
process's imports that the host forwards.

### 2.3 The terminal is a cell grid, not a byte stream

The kernel owns a buffer of cells in linear memory and the renderer draws it. There is no stream
of bytes carrying control codes, because there are no control codes.

Colours and styling are struct fields, cursor addressing is array indexing, a `curses`-style
layout layer is trivial rather than a parser, and there is no escape sequence to mis-parse. The
whole renderer is ~300 lines of JavaScript.

---

## 3. Architecture

```
┌─────────────────────── main thread ────────────────────────┐
│  boot: capability probe, navigator.storage.persist()       │
│  input: KeyboardEvent → {code, mods} → postMessage         │
│  selection: pointer events → cells → clipboard             │
│  render: OffscreenCanvas (transferred to the worker)       │
└───────────────────────────┬────────────────────────────────┘
                            │  postMessage (no SharedArrayBuffer)
┌───────────────────────────┴────────────────────────────────┐
│                   kernel Web Worker                        │
│  ┌──────────────── JS host shim ────────────────────────┐  │
│  │  imports: log, now, present, fs, fs_sync, svc        │  │
│  │  exports: init, wake, tick, key, resize, ref,        │  │
│  │           sys, sys_async, memory                     │  │
│  │  externref table · OPFS handle table · canvas blit   │  │
│  │  compiled-Module cache · worker pool                 │  │
│  └───────────────────────┬──────────────────────────────┘  │
│  ┌───────────────────────┴──── kernel.wasm ─────────────┐  │
│  │  allocator · core types · Task<T> · scheduler        │  │
│  │  Channel<T> · scheduler jobs · CancelToken           │  │
│  │  screen cells · VFS mount table · console · exec     │  │
│  └──────────────────────────────────────────────────────┘  │
└───────────────────────────┬────────────────────────────────┘
                            │  postMessage: bind, step
┌───────────────────────────┴────────────────────────────────┐
│         Web Worker, one process — every program            │
│  imports: env.memory, kernel.sys, kernel.sys_async         │
│  exports: _start, _resume, _alloc, _free                   │
│  own linear memory, own import closure, own memory cap     │
└────────────────────────────────────────────────────────────┘
```

The kernel runs in a Web Worker and communicates by plain `postMessage`. Rendering happens
against an `OffscreenCanvas` transferred into that worker, so the main thread stays free, and a
"reset kernel" button is `worker.terminate()` followed by a reboot. A runaway program hangs
neither: it is a worker of its own (§4.2), and the kernel is merely waiting for a reply it can
stop waiting for.

### 3.1 Toolchain and language subset

Target `wasm32-unknown-unknown`, freestanding. Any clang with the wasm32 target and `wasm-ld`
will do, because it is used purely as a compiler: none of its runtime and none of its headers
are linked or included. Homebrew's `llvm` and `lld` are the local default and Debian's `clang`,
`lld` and `llvm` are what CI installs; nothing is pinned.

```
clang++ --no-default-config --target=wasm32-unknown-unknown -std=gnu++20 -Os \
    -nostdlib -nostdinc++ -fno-exceptions -fno-rtti -fno-threadsafe-statics \
    -mreference-types -mbulk-memory -msign-ext -mmutable-globals -mnontrapping-fptoint \
    -ffunction-sections -fdata-sections \
    -Wl,--no-entry -Wl,--gc-sections -Wl,--stack-first -Wl,-z,stack-size=131072
```

Appendix C explains each flag, and why `--export-dynamic` and `--allow-undefined` are
deliberately absent: adding either back is a regression. libc++'s `<coroutine>` cannot be used
freestanding, so [src/kernel/coroutine.h](../src/kernel/coroutine.h) is a shim over the
`__builtin_coro_*` intrinsics.

**No exceptions, no RTTI.** Errors are values: `Result<T, E>`, propagated through `co_await`
with a `TRY()` macro rather than by unwinding.

### 3.2 The foundation we own

About 1,500 lines that we are glad to control:

- **Allocator** — a bump arena plus size-class free lists over `memory.grow`. Coroutine frames
  go through it, so it must be fast (§8.2).
- **Core types** — `Str` (a UTF-8 view), `String`, `Vec<T>`, `Span<T>`, `Result<T, E>`,
  `Option<T>`, `HashMap<K, V>`.

Nothing else is available: there is no libc, `new` is not used (`heap_new`/`heap_delete` are),
and a namespace-scope global must be trivially destructible, since a non-trivial destructor
needs `__cxa_atexit`. State that needs a constructor lives behind a pointer built on first use,
as `Sched` and the process runtime's `Rt` do.

### 3.3 The core abstraction

```cpp
template <class T> struct Task {           // lazy, movable, awaitable
    struct promise_type { ... };           // symmetric transfer on final_suspend
};

struct Waker { u32 token; };               // handed to JS, comes back later
```

The scheduler is a ready queue of `std::coroutine_handle<>` plus a
`HashMap<u32, coroutine_handle<>>` of suspended tasks keyed by wake token. An awaitable's
`await_suspend` allocates a token, registers the handle, and calls a JS import that tells the
host "notify me on token N". `tick()` is the only thing that ever resumes a coroutine.

### 3.4 The JS boundary

**Wasm exports** (host → kernel):

```
init(heap_base)
wake(token, payload_ptr, payload_len)   // host signals an event
tick(now_ms)                            // drains the ready queue; ms until the next timer, or -1
key(code, mods) -> u32                  // fast path, no allocation; 0 if the ring was full
resize(cols, rows)                      // returns the screen descriptor's address, or 0
ref(slot, obj)                          // host deposits a JS object in the table (§3.7)
sys(pid, op, a0, a1, a2) -> i32         // a process's synchronous syscall (§4.3)
sys_async(pid, op, token, len) -> i32   // a process's asynchronous syscall (§4.3)
```

The last two are not the host's own business: they are an isolated process's two imports, which
the host forwards with the pid it bound into that process's closure. A process therefore cannot
name another — there is no argument for it on its side of the call. Both are entered from JS at
top level, never from inside a kernel import, which is what keeps them as ordinary as `key()`.

`resize` returns where the screen descriptor (§3.5) lives, which is how the host learns the
geometry and the address of the cell array. It is the only call that moves the cells, so it is
where the renderer re-derives its views (§8.4). The kernel clamps the geometry it is given, so
the host reads `cols` and `rows` back out of the descriptor rather than assuming its request was
honoured.

**Wasm imports** (kernel → host), all non-blocking:

```
host_now(), host_log(ptr, len)
host_present(dirty_x, dirty_y, dirty_w, dirty_h)
host_fs(op, token, req)                        // storage, async  (§5.2)
host_fs_sync(op, handle, ptr, len, off) -> i32 // storage, sync   (§5.2)
host_svc(op, token, req, ref)                  // host services, async (§6)
```

Six, and the smoke test asserts exactly these.

Storage and services are **multiplexed rather than named per operation**: one import per
*calling convention*, so a new operation is an enum value on each side rather than a new import,
and the exact-import assertion stays stable while operations are added. `req` is the address of
a `HostRequest` (`src/kernel/hostcall.h`) carrying the string argument, the flags, a reply
buffer, an `aux` word and the status. Both asynchronous interfaces share that record, one orphan
list and one reaper. **The kernel owns the record for as long as the host may touch it**, which
is past a cancelled await, so it outlives its awaiter rather than being freed under the host.

There is no `host_timer`: the kernel owns the timer queue, so `tick()`'s return value says when
the host must call back and one `setTimeout` serves every sleeping task. Compiling,
instantiating and stepping a process are `host_svc` operations rather than imports of their own.

### 3.5 The screen and the keyboard

```cpp
struct Cell { char32_t ch; u8 fg, bg, attrs, reserved; };   // 8 bytes; fg and bg are palette indices
```

The renderer holds a view over the cell array and blits monospace glyphs to the canvas, plus a
cursor. Damage tracking is a dirty rectangle the kernel updates as it writes, passed to
`host_present` once per `tick`. The cursor is drawn, never stored, so moving it dirties the cell
it left as well as the one it entered. The host finds all of it through a descriptor, whose
address `resize` returns:

```cpp
struct Screen {
    u32 magic;                 // 'BSCR', so a mismatched renderer fails loudly (§8.4)
    u32 cols, rows;
    u32 cursor_x, cursor_y;    // cursor_x may equal cols: the wrap is deferred
    u32 cursor_on;
    u32 cells;                 // address of Cell[cols * rows]
};
```

The wrap is deferred, so filling the last column does not scroll the screen on its own. A resize
keeps the rows in use — `0..cursor_y` — dropping from the top when they no longer fit, and lands
them at the top of the new grid. Re-wrapping logical lines needs a per-row continuation bit the
grid does not have, and lands with whichever milestone needs scrollback.

**The layout layer over the grid** is `src/ui/`, four small things rather than a widget toolkit:

- **`Grid`** — cells, a cursor and a damage rectangle, and nothing else. The kernel's screen is
  one; a full-screen program paints another of its own, in its own address space, and blits the
  damaged part across with one syscall (§4.3).
- **`Pane`** — a rectangle with its own coordinates, style and cursor. Every write is clipped to
  it, so a status line cannot scribble on the text above it. It never scrolls, because scrolling
  moves the whole grid.
- **`TextBuf` and `TextView`** — logical lines and a window onto them. `less` and `edit` differ
  in what they do with keys, not in how they scroll.

`src/ui/` is a library a *process binary* links; the kernel does not link it at all. The
alternate screen is the one piece that stays kernel-side, as `FullScreen` in `src/user/tty.h`:
it copies the grid to a heap block, blanks it, and copies it back in its destructor, which is
what gives the shell's screen back when a program is killed — a killed process runs no
destructor of its own.

**Input is symmetric.** A normalised `KeyboardEvent` becomes `{code, mods}`, is posted to the
worker, and lands in a `Channel<Key>`. A printable key carries its Unicode codepoint; named keys
take values above the Unicode range. **No control characters exist anywhere in the system**:
`^C` is `'c'` with the control modifier set, and the reader decides what that means. That is
§2.3 applied to input. Line editing — history, cursor movement, kill-word, completion — is a
userland `LineEditor` coroutine, not a termios state machine in the kernel.

**The focus is not on the canvas.** A software keyboard is raised by a focused *editable*
element, and a canvas is focusable but not editable — so the page holds one, a hidden `<textarea>`
that `web/braam.js` creates and never shows, and that is where every key event arrives. A canvas
that took the focus would be a terminal no tablet could type into. There are therefore **two
sources and one destination**: a `KeyboardEvent` through `normalise()`, and — for a keyboard that
reports no key, which is what a soft keyboard, dictation and every IME do — the text an `input` or
composition event produced, turned into key codes the way a paste is. One rule decides between
them: **the text route runs exactly when the key route did not prevent the default.** Both end as
the same `{code, mods}`, and nothing below the page can tell which one a keystroke came from.

**One receiver on that channel, and it is the console pump** (`src/user/console.h`), which init
spawns and which never ends: something must hold the keyboard while nothing is running, and a
process has no `keys()` at all. A program therefore does not take the keyboard — it **claims a
route through the pump**, and the prompt is no exception.

**Each of the two routes — raw keys and the screen — has one holder at a time, on the kernel,
named by the pid that took it.** A second claim is `Err(Perm)` rather than a nested one, and a
claim clears its route only if it is still the holder, so a parent and a child may die in either
order. Nesting would mean restoring a predecessor that may already be gone. Painting is held to
the same rule: a `ScreenBlit` from a process that does not hold the screen is refused (§4.3).

**`^C` cancels the foreground if there is one, and is delivered to the claimant if there is
not.** The foreground is a set of pids a process arms with `Sys::Fg`, which is what a shell does
for each stage of a pipeline before it waits; the pump cancels them, so a program that has taken
the screen and stopped answering stays killable. With nobody in front the interrupt is an
ordinary key going to whoever holds the raw route — which is what lets a line editor abandon the
line being typed instead of being cancelled by it. Without that split, a shell that is a process
would be killed by its own `^C`.

Everything the pump does not route to a claimant it **cooks**: echo, a line at a time, `^D` for
end of input, into one console channel that is the stdin of whatever is in front. A shell hands
that channel to a child by letting go of the keyboard, which is why `cat` with no argument reads
what is typed.

**Selecting and copying are the page's business, and the kernel is told nothing.** A drag over
the canvas never reaches wasm: `web/braam.js` turns it into device pixels, `web/render.js` turns
those into cells and reverses them as it reverses the cursor, and the text it reads back out of
the grid crosses to the page when the drag settles. There is no mouse event in the ABI, no
selection in the `Screen` descriptor and nothing a program can ask, because a selection is a
*view* over the grid rather than input and the grid is already shared (§2.3). `Ctrl+C` — `Cmd+C`
on a Mac — copies when there is a selection and is `^C` when there is not; copying clears the
selection, so the next one interrupts. The clipboard write happens inside the keydown handler,
because that keystroke is the transient activation permitting it (§A.2). Any other keystroke,
and any resize, drops the selection. Select all is `Cmd+A`, or `Ctrl+Shift+A` where there is no
`Cmd`, and deliberately not `Ctrl+A`, which is the line editor's beginning-of-line. The focus
deciding which terminal on a shared page owns the copy chord and the paste event is the hidden
input's rather than the canvas's; the canvas carries a `braam-focus` class so a page can still
draw a ring around it.

**The key bar is page-side in exactly the sense the selection is.** A software keyboard has no
`Ctrl` and no `Esc`, and usually no `Tab` and no arrows, so a page may hand `mount()` a container
and get a row of buttons for them, `Ctrl` latching onto the next key sent. What comes out is an
ordinary `{code, mods}`: there is no bar, no latch and no touch event anywhere in the ABI, and the
kernel cannot tell a tapped `Esc` from a typed one. The page supplies the keys the hardware does
not have — not a second kind of input.

**A paste is a run of keystrokes, and nothing downstream can tell it from fast typing.** There
is no byte stream to write into (§2.3), so `web/keys.js` turns the pasted text into key codes —
one `Enter` per newline, `Tab` for a tab, nothing for a control character no key produces — and
the worker feeds them through `key()`. That is why a paste needs no import, export or syscall of
its own. What a run does need is **back-pressure**, and that is the whole reason `key()` returns
something: the ring holds 64 keystrokes, so the host feeds a paste at the rate the console
drains it rather than losing the tail. That return value reports a fact the host cannot
otherwise observe; it is not an answer arriving from the kernel, so §2.2 is untouched.

**Soft-keyboard, dictation and IME text take that same road**, and for the same two reasons: the
ring's back-pressure, since a dictated sentence arrives all at once, and ordering — a `key()` is
dispatched ahead of a run still being fed, deliberately, so that `^C` never waits behind a paste,
which means a backspace posted as a key could overtake the word it follows. So everything the text
route produces, backspace and `Enter` included, is fed as a run. The one exception is the
character after a latched `Ctrl`, which wants to jump that queue precisely because it is `^C`.

`Cmd+V`, or `Ctrl+V` where that is the chord, is the browser's own gesture and is not prevented,
precisely so the `paste` event is produced. That event is the document's rather than the
canvas's, so a terminal claims one only while it holds the focus. Within the terminal that has
the focus, a `pbpaste` waiting for the same gesture (§6) takes the text and nothing is typed.

### 3.6 Kernel objects

- **`Channel<T>`** — an async MPSC queue with bounded capacity: `co_await ch.recv()` and
  `co_await ch.send(v)`. This one type is the pipe, the stdin and the IPC. It has **one
  receiver**, and panics on a second blocked sender rather than losing a wakeup quietly.
- **A scheduler job** — a `Task<i32>`, a name and a `CancelToken`, which is all the scheduler
  keeps. `sched_spawn()` pushes one on and hands back its pid; killing means signalling the
  token, and every `co_await` point checks it and unwinds by returning, so destructors run.
  `sched_procs()` reads the table back out, which is what `/proc` is made of (§5.1). There is no
  `Process` type: argv and stdio belong to the pipeline stage (`src/user/prog.h`) and a working
  directory to the process record `exec` keeps.

  **Cancellation does not propagate down a tree.** `CancelState::waiting` is a single slot, so a
  job cannot have two children parked at once — which a pipeline needs, since its stages run at
  the same time. So a pipeline's stages are independent jobs and §3.6's parent-child
  relationship is put back by hand: `run_line`'s frame holds a destructor that cancels every
  stage it started, on its way out for any reason. The cost is that a cancelled child does not
  unwind until the scheduler resumes it, a tick or two after its parent is gone, so it must
  touch nothing the parent owns. A real child-group awaitable needs intrusive queue links inside
  `Waiter` first.
- **Filesystem** — an async node tree, not inodes. One interface, split by *when* the work can
  happen rather than by what it does: naming a file may need the host and therefore a wake
  token, but an already-open file does not (§5.2).

  ```cpp
  struct Fs {
      virtual Str kind() const;                     // what `mount` prints
      virtual bool writable() const;
      virtual u64 bytes() const;                    // for `df`; 0 when it cannot know

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

  `read` fills a caller's buffer rather than returning a `Bytes`, because nothing below this
  line owns a buffer the caller can keep. An implementation sees paths already resolved and
  relative to its own mount point, so it never has to know where it was mounted. A mount table
  maps prefix → `Fs`, longest prefix winning, and an open-file table above it holds the
  descriptors. Implementations in §5.1.
- **Programs are not kernel objects.** One file in `src/cmd/`, built into a binary of its own
  (§4). The job table is not one either: it is the shell process's own memory.

### 3.7 Holding JS objects

A `Response`, a `FileSystemFileHandle` or a `WebSocket` is held in an **`externref` table** as a
slot index, with no serialisation, wrapped in an RAII `JsRef` that frees the slot in its
destructor.

**The table is the kernel's, and JS never indexes it.** `import_module`/`import_name` apply to
functions only, so a table cannot be imported; a module-defined one is what the toolchain
supports. The traffic therefore runs this way round:

- The kernel reserves a slot and publishes the number in the request record.
- The host, when its promise resolves, calls the `ref(slot, obj)` export to deposit the object.
- To *use* it, the kernel reads the slot and passes the object as `host_svc`'s fourth argument.
  JS sees the object, never the table.

An instance's table is part of the instance, so a process can only reach the objects its own
kernel put there. A slot is owned: `JsRef` is move-only, and a request that reserved one owns it
until `await_resume` hands it over, so a cancelled request frees the slot along with the record.
A slot the host deposits into belongs to the record rather than to a frame (`reserve_ref()`).
Releasing a *service* object additionally tells the host to let go — a socket has event handlers
holding it alive on the JS side — which is what `JsHandle` in `src/svc/svc.h` adds.

OPFS handles are not in this table: a filesystem handle is only ever an integer on the wasm
side, so it is a plain JS array indexed by slot number.

---

## 4. Process model

**Every program is a binary in its own instance, in a worker of its own.** There is no in-kernel
program, no program registry and no way to write one. A program gets its own address space, its
own capabilities, its own descriptors and a memory cap the kernel sets, inside a Web Worker
holding nothing else — so `worker.terminate()` ends it without its cooperation. There is nowhere
else to put a process: `braam_add_program` arranges it unasked, and the binary's `braam` section
carries a memory cap and an ABI number but no placement flag (§4.3).

**A host with no worker to give is waited out, not worked around.** Where the constructor throws
— a browser without nested workers, a host disposing of its pool — the spawn is refused with
`Error::Again` and `spawn_process` (`src/user/exec.cpp`) backs off 10, 20, 50, 100, 200, 500 ms
and then a second indefinitely, printing `no worker, retrying` on the program's own stderr. It
is an ordinary await, so `^C` abandons it. Nothing is latched, so a host that recovers is
noticed. The alternative — instantiating in the kernel's worker — is a process with no kill
switch sharing the kernel's liveness, and a browser that cannot make a nested worker cannot run
Braam.

**The shell is not an exception.** `/bin/sh` is a binary in `/bin` that init runs, and
everything a prompt needs — a pipeline, a redirection, a job, a working directory, the keyboard,
the cursor — it asks for through §4.3. What is left inside the kernel is not a weaker kind of
process: it is the dispatcher those requests arrive at.

**A process that loses its worker dies with it, and init replaces the shell.** There is no
moving a running process; the instance went with the worker. Init starts another `/bin/sh` when
its shell **died** — a trap, a failed step, an instance that would not be made — and not when it
**exited**, which is the user's own `exit` and the end of the session. It is bounded at three
deaths in quick succession; a shell *waiting* for a worker is not one, since it has not started.
A replaced shell is a fresh one: kernel `/home`, empty job table.

**A shell builtin is not a program and has no file in `/bin`**, but it is not kernel code
either: `cd`, `fg`, `jobs`, `kill`, `help` and `exit` live inside `/bin/sh`, in
`src/sh/builtin/`. What makes one a builtin is that it touches the shell *process's own* state —
its working directory, which a typed command inherits at spawn; its job table, which no syscall
shows anyone; its loop. It pipes and redirects through descriptors like anything else, but runs
**in its turn rather than alongside**, since nothing inside a process can wait for a sibling
task. So **a builtin buffers its output and writes it once**: one that wrote a line at a time
would fill an eight-slot pipe and park with nobody left to drain it.

### 4.1 What separate instances buy

- **Address space: isolated, for free.** Two instances have two `WebAssembly.Memory` objects,
  and no instruction reaches outside your own linear memory. A wasm pointer is an offset, not an
  address; there is nothing to forge. This is stronger than MMU-based isolation, because it is
  enforced by the type system and bounds checks rather than by page tables one might
  misconfigure.
- **Capabilities: isolated, if we are careful.** An instance can only call the imports supplied
  at instantiation. Each closure is bound to its pid, so process 7 physically cannot issue a
  syscall as process 3: it holds no function that does so. The same applies to the `externref`
  table (§3.7).
- **Memory limits: isolated, and a bonus.** `new WebAssembly.Memory({initial, maximum})` is a
  hard ceiling — 256 pages, 16 MB — and `memory.grow` simply fails past it. That is an rlimit
  without cgroups. When a process ends the instance is dropped and *all* its memory returns at
  once.

### 4.2 What they do not buy: CPU time

**`while(1){}` cannot be preempted.** Nothing in the wasm specification allows it, so
address-space isolation and *liveness* isolation are separate problems. One worker per process
makes `worker.terminate()` the `SIGKILL`, which is what an operating system owes its user, and
it needs no metering: a step is one more asynchronous host request, so a process that never
answers is a request that never lands, and `^C`, `kill` and a cancelled job already know what to
do with one of those. **Fuel counters** — a binary-rewriting pass injecting
`if (--fuel < 0) trap;` at loop headers — remain the only way to *bound* CPU rather than end it,
and are unbuilt.

Workers are hired from a small pool, sized for a pipeline *above* what the session holds
permanently, since the shell is one of these processes and never gives its worker back. A worker
that has finished its process is clean — the instance is dropped and wasm cannot have touched
the worker's own scope — so it goes back to the pool. One that was terminated is gone, which is
the point.

### 4.3 The kernel↔process ABI

`src/kernel/sysabi.h` is the wire, included by both ends so neither can drift alone. The `abi`
word in the custom section is what makes an amendment safe: `exec` refuses a binary whose number
is not the kernel's, so a stale binary is a diagnostic rather than a wrong answer.

```
process imports:  env.memory                        // the kernel's, so the cap is the kernel's
                  sys(op, a0, a1, a2) -> i32        // sync ops, immediate result
                  sys_async(op, token, ptr, len)    // async ops, reply via _resume

process exports:  _start(argv_ptr, argv_len) -> i32 // 0 = exited, 1 = suspended
                  _resume(token, ptr, len)   -> i32 // the same
                  _alloc(n) -> ptr, _free(ptr, n)

custom section "braam":  magic, abi, flags, initial_pages, max_pages
```

The coroutine model survives the boundary intact: the process's `co_await` suspends, its
scheduler returns control out through `_start`/`_resume`, the kernel continues, and later calls
`_resume` with the payload. Reentrant scheduling across an instance boundary, with no stack
switching.

The wire's conventions:

- **Memory is imported rather than exported**, with no declared maximum, so §4.1's ceiling is
  the kernel's decision and not a number the binary could have written differently.
- **`_start` takes argv rather than argc**, because the host places the blob through `_alloc`
  and `argc` alone cannot say where it put it. The blob is `u32 argc`, then a length and bytes
  per word.
- **A reply payload begins with an `i32` status**: `_resume`'s signature has room for a buffer
  and not for an errno, and every asynchronous syscall needs both.
- **The op word's upper bits are the operation's argument** — a descriptor, the open flags, or
  one small immediate — so a payload is only ever the operation's *data*, with no header glued
  on the front.
- **A process may have several syscalls outstanding** — one per task, and `PROC_TASKS` is eight
  — and the step request's `flags` says which one a reply answers. Kernel-side each parked call
  is a record with its own staging block and its own scheduler job, so a socket read that never
  completes cannot starve the keystroke behind it.

**The table is thirty-six operations and `PROC_ABI` is 8**: four synchronous — `exit`, `getpid`,
`now`, `stage` — and thirty-two asynchronous.
[System_Calls.md](System_Calls.md) lists them all with what each carries.

Four rules bound the table:

- **Every operation has a caller in `src/cmd/`.** That is a rule against *growing* the table on
  speculation, not one that retires an operation whose caller is refactored.
- **The synchronous half is closed at four.** Each is answerable inside the process's own worker
  with no kernel to ask — `getpid` from the closure, `now` from the step message's clock plus
  elapsed time, `exit` buffered onto the step's reply, `stage` refused with the "no room" answer
  the runtime already handles — which is the whole reason one binary runs there at all. A fifth
  would have nothing to answer with and would fail in a worker alone. So an operation that needs
  the kernel is asynchronous whatever it costs, including a `wait` on a child that has already
  exited.
- **A stream of bytes comes back as a descriptor**, so `read`, `write` and `close` serve it and
  nothing is duplicated: a fetched body is read like a file, a socket is written like one, and a
  killed process drops all of them with its handle table.
- **What the kernel publishes as text needs no operation.** `/proc` is a filesystem, so `mount`
  is `/proc/mounts` and `cat` and `grep` are the introspection tools. `pwd` is the one thing
  that argument cannot reach — ProcFs generates a file at `open` and has no idea who is reading
  — so `chdir` is an operation, because "which process is asking" is not a question a filesystem
  can be asked.

**A descriptor named in a spawn is moved, not duplicated.** The parent's slot is closed and the
child owns it. POSIX duplicates and expects the parent to close its copy, and forgetting is the
classic bug where the reader never sees end of input. Moving makes it unrepresentable — and a
`Channel` has one receiver and panics on a second blocked sender (§3.6), so two processes
holding one pipe end would be a user program reaching a kernel invariant. A descriptor a syscall
of the parent is parked on cannot be moved at all, and a spawn refused on any slot takes none of
them. Within a process, a second concurrent use in the same direction is `Err(Perm)`.

**A child is an ordinary scheduler job**, spawned exactly as a pipeline stage is, so `^C`,
`kill`, `jobs` and `/proc` reach it with nothing added. Its parent's destructor cancels it,
which is §3.6's structured concurrency put back by hand one level further down; its status is
recorded on the parent's record by a destructor that finds the parent by pid, since pids are
never reused. Both bounds — `SYS_CHILD_MAX` live children, `SYS_PROC_DEPTH` levels deep — hold
because every child is an instance with a memory cap of its own, and nothing else would stop the
first fork bomb.

**0 is not a pid.** It is `sched_spawn`'s failure return, what `tty_keys_owner()` and
`tty_screen_owner()` mean by "nobody", `SYS_WAIT_ANY`, `Fg(0)`, and `link.pid = 0` in
`web/proc.js`. `/bin/sh` takes init's pid, since it is a process inside init's task rather than
a job of its own.

**`Sys::Fg` is authorised the way `kill` is, and then some**: the pid must be a child of the
caller, and the caller must have the terminal already — it holds the raw keys, or it is itself
in front, or nobody is, or **what is in front is what it put there**. The last two clauses are
not slack. A shell must let go of the keyboard *before* it spawns, because a child runs as soon
as the shell next parks and a full-screen program claims the keys in its first step. And it arms
a pipeline a stage at a time, so from the second call onwards it holds neither the keys nor a
place in the set it is filling. The foreground therefore belongs to whoever armed it, and the
console records that rather than inferring it.

**A repaint is one operation.** `Sys::Echo` carries an anchor, a cursor offset, a sequence of
styled runs and the bytes; its reply says where the cursor ended, what the geometry is, and how
many rows the write carried the anchor up. It authorises nothing `Sys::Style`, `Sys::Cursor` and
`Sys::Write` do not, and every byte still goes out through the process's own stdout, so a
redirection behaves. It exists because §4.4's cost falls per *operation*: a keystroke was five
round trips and is two, and Enter to the next prompt was twelve and is five. Being one operation
also keeps the intermediate states off the screen, since the grid is presented once per tick.

**The kernel does not call a process; the host does, and never with the kernel on the stack.**
Only JS can call another instance's exports, and re-entering the kernel from inside one of its
own imports would run it on a heap it is halfway through changing. So one `_start` or `_resume`
is a *deferred host action*, structurally identical to a storage reply: the process's proxy task
parks on a wake token, the host steps the instance once the tick has unwound, and the token is
woken with the outcome. Synchronous syscalls run the other way and re-enter the kernel at top
level, exactly as `key()` does.

**What crosses is bytes, not addresses** (Appendix B). The host asks the kernel for room with
`Sys::Stage`, copies the payload in, and only then reports the request; the reply travels back
through a block the host takes from the process's own `_alloc`. One message each way per step,
and both halves of that protocol live in `web/proc.js` — `serveProc` is the process's side,
`makeProc` the host's — because two files describing one wire is how it drifts.

**Whoever takes a worker away must fail the in-flight step**, or the kernel parks for ever on a
reply that is not coming. An abandoned request is reaped by `wake()` on its token and by nothing
else, which is why `sched_wake` returns a bool.

A trap is how a process reports a fatal error: it has no host imports to log through, so the
kernel turns a trap into an exit status and says the process crashed. Two fidelity losses come
with the worker and neither is worth an ABI change: a binary that will not instantiate reads as
a crash (132) rather than as "will not instantiate" (126), and `Sys::Now` is relative.

**A process ends when its root task returns**, whatever the others are doing. The kernel then
drops the instance and cancels the servers of anything still outstanding.

### 4.4 Cost model

Compilation is expensive; instantiation is cheap. The host keeps the `Module` in a cache keyed
by path and instantiates per `exec`. `new WebAssembly.Module(bytes)` is synchronous, which is
allowed in a worker at any size and keeps `exec` one round trip rather than two; the bytes come
from the VFS, so a binary can live in OPFS or `/home` and not only beside `kernel.wasm`.
`Module` objects are structured-cloneable, so a binary is compiled once however many workers run
it, which is why the cache stays in the kernel worker. Starting a worker is the other cost, and
the pool (§4.2) is the answer.

**A syscall is the cost that does not go away**: two `postMessage` hops and two copies,
**measured at 34–45 µs** in three engines. A syscall-bound program pays it per `SYS_CHUNK` (512
bytes) — a quarter of a megabyte through three processes is 6–13 ms — which is why a bigger
chunk or a batched step protocol was decided against. What that leaves on the interactive path
is the *line* rather than the key: a keystroke is two round trips and Enter to the next prompt
is five, paid once a line, which is why it is affordable.

**The real cost is duplication.** With no dynamic linking, every binary embeds its own copy of
the allocator, the string types and the coroutine runtime; the staged tree is ~491 KB and
`sh.wasm` is 81 KB of it. Keep the process-side runtime minimal and push anything substantial
into syscalls, so it lives once in the kernel rather than N times in userland. That *tree*
carries a size budget and the individual binaries do not, so that number is where the
duplication stays visible — `rootfs.zip` is deflated, and its own size would hide it.

Cross-instance data movement is Appendix B.

---

## 5. Storage

Appendix A has the full comparison of browser storage APIs and the durability caveats.

### 5.1 The mount layering

```
OpfsFs     → OPFS                (/, and therefore everything — the store)
ProcFs     → the scheduler       (/proc, generated at open)

unbuilt:
  a File System Access Fs        (a real local directory, Chromium only, opt-in — §5.4)
  an Fs over Range requests      (read-only remote trees)
```

**Two mounts, and one of them is generated.** Everything a user can name is in the one store:
`/bin`, `/share`, `/home`, `/tmp` and `/mnt/import` are directories in it, not filesystems of
their own. There is no `/usr`, and `import` writes the picker's bytes into `/mnt/import` like
anything else — bytes are not a filesystem.

`/bin` and `/share` are put there at boot by unpacking `rootfs.zip`, an ordinary deflated zip
beside `kernel.wasm` that `tools/pack.py` builds and `web/fs.js` reads; the kernel never sees
its bytes. They are therefore **writable**, which is the price of one store: `/bin` used to be
immutable because it was a read-only archive mount, and what stands in for that now is that the
archive can always be unpacked again (§5.2).

`/proc` is `ProcFs` over the scheduler: `cwd`, `meminfo`, `mounts`, `uptime`, `version`, and one
file per live pid. It is also why the process ABI is as small as it is (§4.3) — a process reads
its answers here rather than asking for an operation — and it makes `cat` and `grep` the
introspection tools, with no second interface to keep in step. The tree is flat: a process here
has one line of state, and a generated directory level would hold exactly one file. Content is
produced at `open` and read out of that snapshot, so a two-block read cannot describe two
different moments. `/proc/cwd` is the *kernel's* working directory; every process's own is a
line in its own `/proc/<pid>`. There is no `/proc/jobs`, because the job table is a process's
memory and no syscall shows one process another's.

**Every process has a working directory of its own**, inherited from whoever spawned it and
moved only by its own `chdir`. The shell's is the shell process's; `cd` moves that, and a typed
command inherits it at spawn — which is what a redirection on that line is relative to, since
the shell opens those itself before any stage runs. A `cd` in one process moves nobody else's
feet, and that is the whole of why `cd` is a builtin. The kernel keeps one for itself, which is
where init resolves `/bin/sh` from.

What a process is *not* isolated in is the namespace: there is no per-process root, and `open`
resolves with the kernel's full authority once the path is absolute.

### 5.2 OPFS is the primary store

The Origin Private File System is private to the origin, invisible in the user's regular
filesystem, and supported by Safari, Chrome, Edge and Firefox. It gives real directory handles,
real file handles, seekable reads and writes, truncate, rename and remove — which maps onto §3.6's
`Fs` almost one-to-one.

The detail that matters most: the high-performance **synchronous** `read()`/`write()` methods
obtained via `createSyncAccessHandle()` are exposed **only inside a Web Worker** — not the main
thread, not an iframe, not even a SharedWorker. The kernel already lives in a worker, so the
fast path comes free. **Opening** a file is async (one wake token), but once a sync access handle
is held, `read`/`write`/`getSize`/`truncate`/`flush` return immediately: those are plain
value-returning imports, the second sanctioned exception to §2.2.

**The store is unpacked from `rootfs.zip`, and stamped.** An empty store is filled at boot
without asking; after that `/version` holds the `BRAAM_VERSION` of the kernel that wrote it, and
boot compares it against its own. A mismatch is the user's decision — the prompt is on the grid
before the shell, since a stale `/bin` may be exactly what they want kept — and declining boots
on what is stored. The unpack replaces the top-level directories the archive carries, `bin` and
`share`, and never names any other, so `/home` cannot be lost to one. The stamp is written last,
so an interrupted unpack is done again rather than believed.

That is also what a writable `/bin` is held up by. `rm /bin/sh` is reachable from the prompt and
the stamp would still match, so `no_shell` offers the unpack again rather than leaving an origin
that can never boot. **The archive, not the store, is the thing the system can be recovered
from** — which is why it is fetched lazily and never cached into the store as bytes.

Two constraints to build around:

- A sync access handle takes an **exclusive lock**, so the VFS needs an open-file table. The
  table holds **one backend handle per file and shares it**: a second open takes a reference on
  the handle that is already there rather than asking OPFS for one it would refuse. Offsets live
  above the VFS, so descriptors sharing a handle cannot disturb each other's position. What the
  table still refuses is a second opener while a *writer* holds the file, and a writer while
  anyone holds it — `O_TRUNC` counts as writing, since a share skips the backend open that would
  have performed it. Sharing is what makes that one rule on every backend: none of them is ever
  asked to open a file twice, so the rule cannot depend on which mount a path landed in.
- OPFS is unavailable in Safari private browsing. Capability-detect and **stop**: with the whole
  namespace in one store there is nothing to fall back to, and a memory namespace that looks
  like a system until the tab is reloaded is worse than a refusal. Boot says so on the grid and
  starts no shell.

### 5.3 Capability struct, not probing

The kernel asks once, at boot, and keeps the answer:

```cpp
struct StorageBackend {
    bool opfs, sync, fsaccess, persisted;
    u64  quota, usage;
};
```

`mount` consults this rather than probing at use time. It arrives as the reply to one `Info`
operation rather than being pushed in by a separate export, which keeps the boundary to the two
imports of §3.4 and lets `df` ask again for a fresh `usage` instead of reporting a boot-time
snapshot.

`persisted` is the one field the worker cannot obtain: `navigator.storage.persist()` exists only
on the main thread (§A.2). The page calls it during boot and posts the answer down, and the
worker's boot waits for it — reporting the wrong durability is worse than a tick of delay. The
wait is *bounded*, because the call is not always a tick: the page sends a provisional
best-effort answer if the browser has not decided within a grace period, and the real answer
after it, which corrects the store. The request is made once per page however many terminals are
mounted, since persistence belongs to the origin.

`df` reports the backend, the mode, the quota and the usage, so storage semantics are
inspectable from inside the OS instead of being invisible browser behaviour.

### 5.4 The real local filesystem, and the escape hatch

`showDirectoryPicker()` yields a handle to an actual folder on disk, read-write, after an
explicit user gesture and permission grant. Its reach is limited (§A.3), so it is strictly
progressive enhancement, offered only where `window.showDirectoryPicker` is defined. Directory
handles are structured-cloneable, so one can be stashed in IndexedDB and the mount re-offered on
the next visit, though permission must be re-requested each session.

**This is unbuilt.** `mount` is an ordinary binary that reformats `/proc/mounts`, and mounting is
not something a user does: `vfs_mount` is called from boot and nowhere else. Making it one needs
the `Fs` above, a syscall or a `/proc` write to reach it, and an answer to what a second process
should see — the namespace question §5.1 leaves open.

The universally available escape hatch is the boring one, and it is built: `<input type="file">`
for import and a Blob download for export, as `/mnt/import` and the `import`/`export` commands.
Both live on the **page** rather than in the worker, because a file picker and a download need
the DOM. The picker opens inside the transient activation of the keystroke that ran the command,
which is why `import` works without a button of its own.

---

## 6. Host services

Everything the browser offers that is not storage and not the terminal reaches the kernel
through the one `host_svc` import (§3.4), as an operation on the shared request record with the
object it acts on passed alongside as an `externref`. Naming an import per operation is not the
style: a new service is an enum value on each side.

- **`fetch`** — the body comes back as a descriptor, so `read` and `close` serve it and nothing
  is duplicated.
- **WebSocket** — likewise a descriptor, written like a file.
- **The clipboard** — read and write.
- **File transfer** — the picker, opening one of its files, and a save.
- **The wall clock** — milliseconds since the epoch and the browser's offset from UTC.
  `Sys::Now` is monotonic and cannot name a day, so `date` needs this.
- **Process operations** — compiling a binary, instantiating it in a worker, stepping it and
  killing it (§4.3). They are asynchronous operations on the host, which is this convention
  exactly, so they are operations here rather than an interface of their own; `aux` in the
  request record is the pid they name.

Every one of them is a promise on the host side, so every one takes a wake token and §2.2 is
untouched. The wall clock is the near miss — `Date.now()` is as synchronous as `host_now()` —
but a service already had an import, and one more operation on it costs nothing while a second
value-returning import would cost the invariant.

The clipboard, the picker and the download need the DOM, so `web/svc.js` relays those across
`postMessage` and answers by id. That is invisible from the kernel: a service operation is a
token either way.

**Reading the clipboard does not fit the pattern, and cannot.** `navigator.clipboard.readText()`
is only permitted from inside a user-gesture handler, and a command's request reaches the page
after the keystroke's handler has returned, so the call is never in one. Safari refuses, Firefox
does not offer it to page content, and Chrome prompts. The way out is that **a paste is itself
the gesture**: the `paste` event hands the text to the page with no permission at all, in every
browser. So a refused read becomes a wait for one, and `pbpaste` says so rather than failing.
That is why `Ctrl+V` is on the reserved list in `web/keys.js` — the kernel must not eat the
keystroke that produces the event (§3.5).

---

## 7. Repository layout

`braam_fs` and `braam_svc` are siblings above the kernel and below userland, depending on
neither the other nor upwards. `braam_ui` is in neither hierarchy: it is linked by `braam_proc`
and *not by the kernel at all*, because the programs that paint are binaries. `src/proc` is a
*different binary's* runtime — what it shares with the kernel is four translation units and a
handful of headers.

```
doc/Concept.md          this document
doc/Release_Notes.md    reasoning behind the code, and M0–M9's acceptance criteria
doc/System_Calls.md     the kernel↔process mechanism, end to end (§4.3)
doc/Programming_Manual.md  the SDK's guide
Makefile                wrapper: all, run, serve, install, release, clean
CMakeLists.txt          the build
cmake/                  the wasm32-unknown-unknown toolchain file, BraamProgram.cmake
src/kernel/             allocator, core types, Task, scheduler, Channel, screen
src/kernel/coroutine.h  the freestanding <coroutine> shim (Appendix C)
src/kernel/hostcall.h   the asynchronous host request, shared by both interfaces
src/kernel/jsref.h      the externref table and JsRef (§3.7)
src/kernel/sysabi.h     the kernel↔process wire, included by both sides (§4.3)
src/proc/               a process binary's whole runtime: _start, syscalls, stdio
src/cmd/                one file per program; every program is a binary of its own
src/fs/                 Fs interface, path, VFS, OpfsFs, storage ABI
src/svc/                fetch, WebSocket, clipboard, file transfer, clock, processes (§6)
src/ui/                 the layout layer over a Grid: Pane, TextBuf, TextView (§3.5)
src/user/               exec and the syscall dispatcher, the console and its pump, the
                        pipes behind a stage's stdio, ProcFs, boot and init
src/user/tty.h          the terminal claims: KeyInput, FullScreen
src/sh/                 the shell: grammar, LineEditor, job runtime, builtins
src/cmd/sh.cpp          its entry point — /bin/sh is a binary like any other
bundle/                 the tree tools/pack.py packs into /bin and /share
examples/hello/         the SDK's worked example, and an ordinary build target
test/                   in-wasm unit tests, the Node driver, and the fakes: storage,
                        services, and a process worker with no thread in it
web/                    braam.js (the embedding API), worker.js, host shim, renderer
web/proc.js             both halves of the process protocol; procworker.js is one
                        process's worker, and wiring only
tools/                  build scripts, bundle packer, metadata stamper, version and
                        release scripts, size-budget check, chat server
```

---

## 8. Things to get right

### 8.1 Every awaitable is cancellation-aware
Retrofitting cancellation into coroutine code is painful. `CancelToken` participates in every
`await_suspend`, and every awaiter deregisters in its destructor (`sched_unwait` from
`~Awaiter`), which is what makes destroying a suspended frame safe. A parking awaitable with no
destructor is a use-after-free.

### 8.2 Coroutine frame allocation is the hot path
Frames are heap-allocated per call, so the allocator is built with this as its primary workload.
A frame past 512 bytes costs a whole 64 KiB span, the allocator's top size class, so long-lived
state belongs in a heap block the frame points at rather than in the frame.

### 8.3 Never let an import return data synchronously
Beyond the two documented exceptions (§2.2). One exception is pragmatic; three are a second ABI.

### 8.4 `memory.grow` detaches the `ArrayBuffer`
Any cached `Uint8Array` view goes dead after a growth. Route JS-side access through a `view()`
accessor that re-derives, and make a mismatch fail loudly — the `Screen` magic word is there for
this. A host request may likewise outlive the coroutine that issued it, so anything whose
address crosses to JS is a heap record the kernel keeps alive past a cancelled await, never a
frame buffer.

### 8.5 Safari's 7-day eviction is a real hazard
With cross-site tracking prevention on, an origin that sees no user interaction for seven days
of browser use has all script-created data deleted. Mitigations: request persistence, encourage
Add to Home Screen (installed web apps are exempt from the ITP timer), and make `export` easy.

---

## Appendix A — Browser storage APIs

### A.1 The tiers

| API | Shape | Where it runs | Our use |
|---|---|---|---|
| **OPFS** | Real files and directories, origin-private, invisible to the user | Async API anywhere; **sync** handles worker-only | **Primary store** |
| **IndexedDB** | Async key → blob, transactional | Anywhere | Metadata; stashed directory handles |
| **Cache API** | Request → Response pairs | Anywhere | Unused: rootfs.zip is fetched at most once per version |
| **localStorage** | 5 MB, sync, strings only | Main thread only | Tiny config, nothing else |
| **File System Access** | The *actual* user disk, with a picker | Chromium desktop only | Optional, unbuilt (§5.4) |

### A.2 Durability

Storage is **best-effort by default**, meaning it can be deleted without asking.

- An origin can opt into persistent mode via `navigator.storage.persist()`, after which data is
  evicted only if the user chooses to delete it. **`persist()` is not available in Web
  Workers** — the main thread calls it during boot and passes the result down (§5.3).
- Quotas are generous but finite. Firefox gives best-effort origins the smaller of 10% of disk
  or a 10 GiB per-site-group limit, and persistent ones up to 50% of disk capped at 8 TiB;
  Safari's overall quota for a browser app is up to 80% of total disk.
  `navigator.storage.estimate()` is surfaced as `df`.
- **Eviction is all-or-nothing per origin.** If it fires, OPFS *and* IndexedDB *and* Cache go
  together, so there is no point using one as a backup of another.

### A.3 File System Access reach

Firefox and Safari ship only OPFS, and no mobile browser exposes the pickers. Safari supports
none of `showOpenFilePicker`, `showSaveFilePicker` or `showDirectoryPicker` on macOS, iPadOS or
iOS. Hence progressive enhancement only, never a dependency.

---

## Appendix B — Cross-instance data movement

Instances cannot call each other, so every transfer is a copy through the host. The kernel
cannot be handed a buffer it did not allocate, so the host asks for one: `Sys::Stage` is a
synchronous syscall the *host* issues on the process's behalf, returning the address of a
staging block the process's kernel-side record owns. The reverse direction needs no such call,
because `_alloc` is already in the ABI.

A process is a worker away, so the copy is in two halves with a `postMessage` between them: the
process's worker `slice`s the payload out into a transferable `ArrayBuffer`, and the kernel's
worker copies that into the staging block. `slice` rather than `subarray` is load-bearing on
both sides — a view is detached by the next `memory.grow` (§8.4), and one that has been
transferred cannot be re-derived. The kernel half is two lines inside the per-pid `sys_async`
closure in `web/proc.js`.

**If the kernel itself is ever to do the copy, multi-memory is the tool.** A module may declare
several memories, and `memory.copy` moves bytes between two of them. Imports are fixed at
instantiation, so the kernel cannot dynamically import a new process's memory; the trick is a
tiny per-process **bridge module** that imports both memories and exports `copy_in`/`copy_out`,
about 30 bytes of wasm instantiated alongside each process. Check `wasm-feature-detect` rather
than trusting the feature's status.

---

## Appendix C — Toolchain notes

Verified against a stock clang for `wasm32-unknown-unknown`, which has no sysroot of its own.

### C.1 libc++'s `<coroutine>` cannot be used freestanding

It is often said that `<coroutine>` is header-only and compiler-intrinsic, so it works
freestanding as soon as `operator new` exists. That is true of the *language feature* but not of
the header: libc++'s `<coroutine>` includes `__functional/hash.h` → `<cstring>` → `<cmath>`,
which need libc declarations (`size_t`, `memcpy`, `FP_NAN`, …) the bare `wasm32-unknown-unknown`
target has no sysroot for. A distribution that carries a wasm sysroot at all carries it per
target, and none has an `unknown-unknown` variant.

### C.2 The shim

[src/kernel/coroutine.h](../src/kernel/coroutine.h) declares `std::coroutine_traits`,
`std::coroutine_handle<>`, `std::coroutine_handle<P>`, `std::suspend_always` and
`std::suspend_never` over `__builtin_coro_resume`, `__builtin_coro_destroy`,
`__builtin_coro_done`, `__builtin_coro_promise` and `__builtin_coro_noop`. It is 124 lines with
its comments, and a deliberate part of the foundation rather than a workaround.

`std::coroutine_traits` must be *defined*, not merely declared: a forward declaration compiles
until the first coroutine, which then fails to instantiate it.

### C.3 The flags in §3.1

- **`--export-dynamic` is absent**, and adding it back is a regression: it is not a reliable way
  to export, having dropped a plain `extern "C"` function while exporting `operator new`.
  Exports are named individually with `BRAAM_EXPORT` (`export_name`), imports with
  `BRAAM_IMPORT` (`import_module`/`import_name`) — never by linker flag. Either changes the ABI,
  so the expected surface in [test/run.mjs](../test/run.mjs) changes in the same commit.
- **`--allow-undefined` is absent**, so nothing is left to resolve and an accidental libc
  dependency is a link error instead of a runtime trap. `memcpy`/`memset` do not leak in:
  bulk-memory lets LLVM lower them inline. Neither Homebrew nor Debian ships compiler-rt for
  this target, so a needed builtin — 128-bit division, an outlined `memcpy` — is a link error
  too.
- **The wasm features are named, not defaulted**, because which of them the default CPU turns on
  has changed between clang versions: `-mreference-types` for `__externref_t`, `-mbulk-memory`
  for the above, and `-msign-ext -mmutable-globals -mnontrapping-fptoint`. The list is verified
  sufficient by building over `-mcpu=mvp`.
- **`--no-default-config`** suppresses any `bin/clang++.cfg` a distribution ships, which is how
  a sysroot gets injected. **`--stack-first`** puts the shadow stack below the data segment, so
  an overflow traps rather than corrupting globals.
- **`-std=gnu++20`** rather than `c++20`, because `TRY()` is a statement expression.
- **`MinSizeRel`**: at `-O0` a freestanding build calls libcalls nothing provides.

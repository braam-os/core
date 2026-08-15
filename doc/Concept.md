# Braam — Concept

An interactive, CLI-oriented operating system that runs entirely inside a browser tab,
written from scratch in freestanding C++20 and compiled to WebAssembly.

This document is the project's single design reference. It states the goal, sets out the
architecture, and records the decisions we have already made and why. It changes when a
decision changes, and then in the same commit as the code. The working plan —
milestones M0–M9 with their acceptance criteria — is in [Milestones.md](Milestones.md), and
the reasoning behind what landed is in [Release_Notes.md](Release_Notes.md).
[System_Calls.md](System_Calls.md) is derived from this one: it walks §4.3's kernel↔process
mechanism end to end, with the operation table in full. Read it to understand the mechanism;
amend this document to change it.

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
sequence to mis-parse. Rendering is under 200 lines of JavaScript.

---

## 3. Architecture

```
┌─────────────────────── main thread ────────────────────────┐
│  boot: capability probe, navigator.storage.persist()       │
│  input: KeyboardEvent → {code, mods} → postMessage         │
│  render: OffscreenCanvas (transferred to worker)           │
└───────────────────────────┬────────────────────────────────┘
                            │  postMessage (no SharedArrayBuffer)
┌───────────────────────────┴────────────────────────────────┐
│                       Web Worker                           │
│  ┌───────────── JS host shim (~1,600 lines) ─────────────┐ │
│  │  imports: log, now, present, fs, fs_sync, svc         │ │
│  │  exports: init, wake, tick, key, resize, ref,         │ │
│  │           sys, sys_async                              │ │
│  │  externref table · OPFS handle table · canvas blit    │ │
│  └────────────────────┬──────────────────────────────────┘ │
│  ┌────────────────────┴──── kernel.wasm ─────────────────┐ │
│  │  allocator · core types · Task<T> · scheduler         │ │
│  │  Channel<T> · scheduler jobs · CancelToken            │ │
│  │  screen cells · VFS mount table · console · exec      │ │
│  └───────────────────────────────────────────────────────┘ │
│  ┌─────── (M8+) per-process WebAssembly.Instance ────────┐ │
│  │  own linear memory, own import closure, own limits    │ │
│  └───────────────────────────────────────────────────────┘ │
└───────────────────────────┬────────────────────────────────┘
                            │  postMessage: bind, step
┌───────────────────────────┴────────────────────────────────┐
│       (M9+) Web Worker, one untrusted process              │
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
    -Wl,--no-entry -Wl,--gc-sections
```

This command line is verified to compile a coroutine that suspends and resumes. See
Appendix C for the one gotcha — libc++'s `<coroutine>` header cannot be used freestanding,
so we supply our own shim over the `__builtin_coro_*` intrinsics, which is
[src/kernel/coroutine.h](../src/kernel/coroutine.h).

`--export-dynamic` and `--allow-undefined` were on this line as first written and are
deliberately gone; C.3 says why, and putting either back would be a regression.

**No exceptions, no RTTI.** Errors are values: `Result<T, E>`. Propagation through
`co_await` uses a small `TRY()` macro rather than unwinding.

### 3.2 The foundation we own

Roughly 1700 lines of code that we will be glad to control:

- **Allocator** — a bump arena plus size-class free lists over `memory.grow`. Under 400
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
key(code, mods) -> u32                  // fast path, avoids allocation; 0 if the ring was full
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
host_now(), host_log(ptr, len)
host_present(dirty_x, dirty_y, dirty_w, dirty_h)
host_fs(op, token, req)                       // storage, async  (§5.2)
host_fs_sync(op, handle, ptr, len, off) -> i32 // storage, sync   (§5.2)
host_svc(op, token, req, ref)                 // host services, async (§3.7)
```

Six, and the smoke test asserts exactly these. A `host_random(ptr, len)` was on this list as
first written and is **unbuilt**: nothing has needed entropy, and an import nothing calls is an
ABI nothing tests. Adding it would make the count seven everywhere it is quoted, which is the
only reason it is worth a sentence.

A `host_fetch` was on the list too, and that is not what M6 built: naming an import per
operation is the style M5 replaced. `host_svc` carries fetch, WebSocket, the clipboard, file
transfer and the wall clock over the same `req` record `host_fs` uses, with the object the
operation acts on passed alongside as an `externref`.

There is no `host_timer`. The kernel owns the timer queue, so `tick()`'s return value already
says when the host must call back, and one `setTimeout` serves every sleeping task.

Storage is **multiplexed rather than named per operation**, which is what a `host_storage_read`
and a `host_storage_write` became in M5. One import per *calling convention* — one
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

The layout layer over the grid arrived with M7, in `src/ui/`, and it is four small things
rather than a widget toolkit:

- **`Grid`** — cells, a cursor and a damage rectangle, and nothing else. The kernel's screen is
  one; a full-screen program paints another of its own, in its own address space, and blits the
  damaged part across with one syscall (§4.3). That is what makes the layer linkable by a
  process at all.
- **`Pane`** — a rectangle with its own coordinates, style and cursor. Every write is clipped
  to it, so a status line cannot scribble on the text above it. A pane writes cells directly
  and marks them in its `Grid`; it never scrolls, because scrolling moves the whole grid.
- **`TextBuf` and `TextView`** — logical lines and a window onto them. `less` and `edit` differ
  in what they do with keys, not in how they scroll.

`src/ui/` is a library a *process binary* links, and the kernel does not link it at all. The
alternate screen is the one piece that stayed kernel-side, as **`FullScreen`** in
`src/user/tty.h`: it copies the grid to a heap block, blanks it, and copies it back in its
destructor, which is what gives the shell's screen back when a program is killed — and a killed
process runs no destructor of its own, so it could not have been the program's (§4.3).

Input is symmetric: a normalised `KeyboardEvent` becomes `{code, mods}`, is posted to the
worker, and lands in a `Channel<Key>`. A printable key carries its Unicode codepoint; named
keys take values above the Unicode range. **No control characters exist anywhere in the
system**: `^C` is `'c'` with the control modifier set, and the reader decides what that means.
That is §2.3 applied to input. Line editing — history, cursor movement, kill-word, completion —
lives in a **userland** `LineEditor` coroutine, not in the kernel. That is where the "line
discipline" belongs, and it is far nicer as a coroutine than as a termios state machine.

There is exactly one receiver on that channel and it is the **console pump**, which init spawns
and which never ends (`src/user/console.h`). It used to belong to the foreground pipeline and be
spawned per job; it cannot, now that the shell is a program — something has to hold the keyboard
while nothing is running, and a process has no `keys()` at all. A program therefore **does not
take the keyboard — it claims a route through the pump**, and the prompt is no exception: the
shell claims `KeyInput` for raw keys and gives it back around anything it runs.

**Each of the two routes — raw keys and the screen — has one holder at a time, on the kernel,
named by the pid that took it.** A second claim is `Err(Perm)`; it does not nest, and a claim
clears its route only if it is still the holder, so a parent and a child may die in either
order. Nesting would mean restoring a predecessor that has already gone: a freed key ring for
`KeyInput`, and for `FullScreen` a snapshot of the blanked grid the first claimant was painting
— the shell's screen thrown away rather than given back. Painting is held to the same rule: a
`ScreenBlit` from a process that does not hold the screen is refused (§4.3).

**`^C` goes to whatever is in front, and to the claimant when nothing is.** The foreground is a
set of pids a process names with `Sys::Fg`, which is what a shell does for each stage of a
pipeline before it waits; the pump cancels them, and a program that has taken the screen and
stopped answering stays killable. With nobody in front it is an ordinary key, delivered to
whoever holds the raw route — and *that* is what lets a line editor abandon the line being typed
instead of being cancelled by it. A shell that is a process could not exist without the
distinction: it would be killed by its own interrupt.

Everything the pump does not route to a claimant it **cooks**: echo, a line at a time, `^D` for
end of input, into one console channel that is the stdin of whatever is in front. A shell hands
that channel to a child simply by letting go of the keyboard, which is why `cat` with no argument
reads what is typed.

**Selecting and copying are the page's business, and the kernel is told nothing.** A drag over
the canvas never reaches wasm: `web/braam.js` turns it into device pixels, `web/render.js` turns
those into cells and reverses them exactly as it reverses the cursor, and the text it reads back
out of the grid crosses to the page when the drag settles. There is no mouse event in the ABI, no
selection in the `Screen` descriptor and nothing a program can ask — because a selection is a
*view* over the grid rather than input, and the grid is already shared (§2.3). `Ctrl+C` — `Cmd+C`
on a Mac — copies when there is a selection and is `^C` when there is not, since a terminal with
no second copy key must overload it; copying clears the selection, so the next one interrupts.
The clipboard write happens inside the keydown handler because that keystroke is the transient
activation permitting it (§A.2), which is why the page holds the text rather than asking the
worker for it once the chord has arrived. Any other keystroke, and any resize, drops the
selection: the cells it named mean something else the moment the grid moves under it.

**A paste is a run of keystrokes, and nothing downstream can tell it from fast typing.** There
is no byte stream to write into (§2.3), so `web/keys.js` turns the pasted text into key codes —
one `Enter` per newline however the platform spells it, `Tab` for a tab, and nothing at all for
a control character no key produces — and the worker feeds them through `key()` like any other
keystroke. That is also why the paste needs no import, no export and no syscall of its own: the
route it takes is the one the keyboard already has, so it reaches a cooked reader, a claimant of
the raw route, and the line editor alike, each on its own terms.

What a run does need is **back-pressure**, and that is the whole reason `key()` returns
something: the ring holds 64 keystrokes and a paste is routinely longer, so the host feeds it at
the rate the console drains it rather than pushing it in one go and losing the tail. A refusal
means the ring is full and the rest of the run waits for the tick that empties it. This is the
only return value on the input path, and it reports a fact the host cannot otherwise observe —
it is not an answer arriving from the kernel, so §2.2 is untouched.

`Cmd+V`, or `Ctrl+V` where that is the chord, is the browser's own gesture: the page never reads
the clipboard for it, and neither key is prevented, precisely so the `paste` event is produced.
That event is the document's rather than the canvas's, so a terminal claims one only while it
holds the focus — an embedded one must not swallow a paste meant for a field beside it, nor let a
`pbpaste` waiting in the terminal next to it steal one. Within the terminal that has the focus, a
`pbpaste` waiting for the same gesture (§5.4) takes the text and nothing is typed: it asked for
exactly this gesture, and a program reading the clipboard wants the text and not the keystrokes.

**Select all is `Cmd+A`, or `Ctrl+Shift+A` where there is no `Cmd`, and is deliberately not
`Ctrl+A`** — which is the line editor's beginning-of-line and has no "is there a selection?" to
tell the two apart the way copy does. It selects the grid, because there is no scrollback for it
to mean anything else. Trailing blank rows never travel with a selection either way: the grid is
a fixed rectangle and the rows below the last line of output are padding, not content.

### 3.6 Kernel objects

- **`Channel<T>`** — an async MPSC queue with bounded capacity: `co_await ch.recv()` and
  `co_await ch.send(v)`. This one type is our pipe, our stdin, and our IPC.
- **A scheduler job** — a `Task<i32>`, a name and a `CancelToken`, which is all the scheduler
  keeps. `sched_spawn()` pushes one on and hands back its pid; killing means signalling the
  token, and every `co_await` point checks it and unwinds by returning, so destructors run
  correctly. The name is a view the scheduler holds rather than owns, and `sched_procs()` reads
  the table back out — which is what /proc is made of (§5.1). There is no `Process` type: argv
  and stdio belong to the pipeline stage (`src/user/prog.h`) and a working directory to the
  process record `exec` keeps (§5.1), so a job is the smaller thing the kernel actually needs.

  **Cancellation does not propagate down a tree, and this section once said it would.**
  `CancelState::waiting` is a single slot, so a job cannot have two children parked at once —
  which a pipeline needs, since its stages run at the same time. So a pipeline's stages are
  independent jobs, and the parent-child relationship is put back by hand: `run_line`'s frame
  holds a destructor that cancels every stage it started, on its way out for any reason. The
  cost is that a cancelled child does not unwind until the scheduler resumes it, a tick or two
  after its parent is gone, so it must touch nothing the parent owns. A real child-group
  awaitable needs intrusive queue links inside `Waiter` first — the same work a channel with two
  blocked senders would need — and Release_Notes.md's "Structured concurrency, put back by hand"
  is the full argument.
- **The job table is not one of these any more.** It was a kernel object while the shell was
  kernel code; the shell is a process now, so the table is that process's own memory — an id, the
  command text and the stages' pids — and `jobs`, `fg` and `kill` read and signal it without a
  syscall between them. What the kernel keeps is what a syscall must serve: the children on each
  process's record (§4.3). A finished background job is noticed at the next prompt by asking
  `/proc` whether its pids are still there, because a `wait` would park and the prompt has to come
  back either way. There is still no `bg` and no `^Z`: stopping a running coroutine at an
  arbitrary point is the resume-side twin of `CancelToken` and would have to reach every
  awaitable.
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
- **Programs** — one file in `src/cmd/`, built into a binary of its own, so adding a command
  means adding one file and a line naming it. There was a registry of `Task<int>(Args, Stdio)`
  functions here until every program became a binary (§4), and then a table of six shell builtins
  until the shell became one too. Nothing of that shape is left in the kernel. The builtins'
  table went with them and is still written out by hand rather than filled in at static-init
  time — `braam_sh` is an archive, and a registrar nothing references would be dropped by
  `--gc-sections` without a word.

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

**Every program is a binary in its own instance.** There is no in-kernel program and no way to
write one; what a program gets is chosen only between the two isolated tiers, and `exec` picks
between them from a flag in the binary's metadata — userland does not notice.

| Tier | Isolation | Spawn cost | Kill | Used for |
|---|---|---|---|---|
| **Instance, shared worker** | address space + capabilities + memory cap | ~1 ms | cooperative | every program, the shell included |
| **Instance, own worker** | the above + liveness | ~10 ms, few MB | `worker.terminate()` | untrusted or long-running |

**There is no third row, and the shell is not an exception to the two.** `/bin/sh` is a binary
in `/bin` that init runs, and everything a prompt needs — a pipeline, a redirection, a job, a
working directory, the keyboard, the cursor — it asks for through §4.3 like any other program.
What is left inside the kernel is not a weaker tier: it is the dispatcher those requests arrive
at.

`exec` reads the tier out of a binary's `braam` custom section (§4.3): a name in `/bin` is a
binary, and the binary says which tier it wants. One asking for tier 3 still runs at tier 2 where
the host has no worker to put it in — a *fallback*, covering a browser without nested workers
and a `procworker.js` that will not load.

**There was a third tier, and it is gone.** The **kernel applet** — a program as an in-kernel
coroutine, sharing the kernel's heap and its whole authority — was how every program was written
before M8 gave them an alternative, and for a while all three tiers coexisted. They no longer do.
Two program models meant two `Args` types, two `io.h`s and two copies of every filter's logic
waiting to diverge, and the weaker model was the one with no memory cap, no descriptor table and
nothing between a bug and the kernel's heap. So the applets became binaries, the ABI grew to
meet them (§4.3), and `Tier::Retired` keeps the number 1 reserved so a binary stamped by an older
build is refused rather than misread.

A **shell builtin** is still not a program and still has no file in `/bin`, but it is no longer
kernel code: the six live inside `/bin/sh`, in `src/sh/builtin/`. What makes one a builtin has
changed with them. It is not "no syscall could serve it" — `chdir`, `wait` and `kill` all exist.
It is that the thing it touches is *the shell process's own*: `cd` moves the working directory a
typed command inherits (§5.1), `jobs`, `fg` and `kill` read and signal a table no syscall shows to
anybody else, `exit` ends the shell's loop, and `help` lists the rest.

A builtin runs inside the shell rather than as a child, so it pipes and redirects by reading and
writing descriptors like anything else. One discipline goes with that, and it is worth stating
because it is a real constraint rather than a style: **a builtin buffers its output and writes it
once.** Nothing inside a process can wait for a sibling task — the only resumption a task has is a
syscall reply — so a builtin in a pipeline runs to completion in its turn rather than alongside,
and a pipe holds eight chunks. A builtin that wrote a line at a time would fill one and park with
nobody left to drain it.

The cost is paid twice over and is worth naming. Retiring the applet took `kernel.wasm` from
236,965 bytes to under 170,000, because a quarter of it was userland; the same code now ships as
~29 binaries, each carrying its own copy of the allocator, the string types and the coroutine
runtime, and the boot archive grew from 47 KB to some 370 KB for it. That is §4.4's duplication
arriving in full. It buys a memory cap, a descriptor table and a kill switch for every command
the system has, which is the trade this section has always described.

One consequence lands on the tests. The in-wasm unit tests cannot drive a binary — stepping an
instance means returning to the host, and `run_tests()` does that once — so with the shell a
binary too they can drive nothing that runs. What they can still reach is everything below a
program: the console and its claims, the pipes, `/proc`, the VFS, and the grammar, which is pure
and is compiled straight into the suite. Everything else is asserted in `test/run.mjs`, against
the real shell and the real programs.

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
  fragmentation and leak problems an in-kernel program had, sharing the kernel heap.

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

The shape is the one this section fixed in M0; the details amended since are marked below, and
the `abi` word in the custom section is what makes an amendment safe — `exec` refuses a binary
whose number is not the kernel's, so a stale binary is a diagnostic rather than a wrong answer.

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

**The op word's upper bits are the operation's argument** — a descriptor at `write`, `read` and
`close`, the open flags at `open`, one small immediate elsewhere. One convention rather than two,
so a payload is only ever the operation's *data*: a write hands over its bytes and an open hands
over its path, neither with a header glued on the front.

**The operation table grew with the programs.** M8 fixed it at eight — `exit`, `getpid`, `now`
and `stage` synchronously, `write`, `read`, `open` and `close` asynchronously — because those
were what a binary needed when only two were binaries. Retiring the applet tier meant every
program needed what it had reached for directly, and the table grew to twenty-seven: `stat`,
`list`, `mkdir` and `remove` beside the descriptor operations; `sleep` for the timer queue;
`clock` and `storage` for the two host services no file can stand in for; `fetch`, `wsopen`, the
two clipboard operations and `pick`/`pickopen`/`save` for the ones that hand back a stream; and
five terminal operations for a program that takes the whole screen. Every one has a caller in
`src/cmd/`, which is the rule that keeps the table from growing on speculation.
[System_Calls.md](System_Calls.md) lists them all, with what each carries.

**A process can start a process, and that took five more.** The table is thirty-two: `chdir`
beside the filesystem operations, and `pipe`, `spawn`, `wait` and `kill` as a family of their
own. What forced them is that a program whose job is to run another program had nowhere to
live — a builtin is the shell's own frame, so `timeout` and `watch` were unwritable rather than
merely unwritten. Those two were the first callers, and the rule above is why there were two and
not a plausible half-dozen. The shell is the third, and the one that made the family general: a
prompt is a supervisor, and it needs every one of the five.

**And the shell took two: `cursor` and `fg`.** The table is thirty-four. Both exist because a
prompt is a program now, and both are the terminal rather than the process:

- **`cursor`** reports and moves the cursor of the *scrolling* screen, get and set in one
  operation. A line editor writes with `write` — which wraps and scrolls, exactly as it did when
  the editor was kernel code — and then has to know where that landed, because nothing counts
  scrolls. `ScreenBlit` could not serve it: it is refused without the alternate screen, and taking
  the alternate screen blanks the grid the prompt lives in.
- **`fg`** names which of a process's children is in front, and therefore what `^C` reaches. With
  nobody in front the interrupt is delivered to whoever holds the raw keys instead (§3.5). Without
  it a shell that is a process would be cancelled by the interrupt meant for the command it ran,
  which is not a thing that can be worked around from userland.

`fg` is authorised the way `kill` is, and then some: the pid must be a child of the caller, and
the caller must have the terminal already — it holds the raw keys, or it is itself in front, or
nobody is. That last clause is not slack. A shell must let go of the keyboard *before* it spawns,
because a child is a scheduler job that runs as soon as the shell next parks and a full-screen
program claims the keys in its very first step; handing them over afterwards is a race the child
loses.

**A colour took one more: `style`.** The table is thirty-five, and `PROC_ABI` is 5. §2.3 says the
terminal is a cell grid rather than a byte stream, so a colour cannot be written *in* the bytes —
there is no escape sequence to reach for, by design — and a program that has no grid of its own
had no way to name one at all. `style` is that way: two palette indices and the `ATTR_*` bits in
the op word's argument, applying to what `write` paints next. It is sticky grid state, as it is
for the kernel's own writers, so the convention is that whoever sets a colour puts the default
back — and the prompt doing so is also what corrects a program that died mid-colour. It is
refused while another process holds the alternate screen, for `cursor`'s reason: a program with
the alternate screen paints cells and names their colours in them. `/bin/sh` is the caller, and
`ScreenBlit` is why there is not a second one.

All the rest are asynchronous, because the synchronous half is closed and stays closed. That costs a
park and a step even for `wait` on a child that has already exited, which is the cost model of
§4.4 arriving where it always does.

**A descriptor named in a spawn is moved, not duplicated.** The parent's slot is closed and the
child owns it. POSIX duplicates and expects the parent to close its copy, and forgetting is the
classic bug where the reader never sees end of input, because a write end is still open in a
process that will never write. Moving makes that unrepresentable — and it is not only tidiness:
a `Channel` has one receiver and panics on a second blocked sender (§3.6), so two processes
holding one pipe end is a user program reaching a kernel invariant. One end, one owner, by
construction. A descriptor a syscall of the parent is parked on cannot be moved at all — the
parent's reader and the child's stdio would be that second receiver — and a spawn refused on any
slot takes none of them, so the parent's table is as it was. A descriptor also has one user per
direction at a time within a process: a second concurrent read, or write, is `Err(Perm)`, for the
pipe ends because `Channel` says so and for the host kinds because a reply sized twice is not
re-entrant against one object.

**A child is an ordinary scheduler job**, spawned exactly as a pipeline stage is, so `^C`,
`kill`, `jobs` and `/proc` reach it with nothing added. Its parent's destructor cancels it, which
is §3.6's structured concurrency put back by hand a second time, one level further down. Both
bounds on it — eight live children, eight levels deep — are there because every child is an
instance with a memory cap of its own, and nothing else would stop the first fork bomb.

**What the kernel already publishes as text needs no operation.** `/proc` is a filesystem, so a
process reads it with `open` and `read` like anything else: `mount` is `/proc/mounts`, and `df`
needs `storage` only for the three numbers behind a host round trip, because ProcFs generates its
files synchronously and asking the host is not. Adding a line to `/proc` is cheaper than adding a
syscall and leaves `cat` and `grep` able to read it.

There is one thing this argument cannot reach, and `pwd` is it. ProcFs generates a file at
`open` and has no idea who is reading, so `/proc/cwd` can only ever be one answer — and once
every process has a working directory of its own, the one answer is the shell's. That is why
`chdir` is an operation and `pwd` calls it: not because the text was expensive, but because
"which process is asking" is not a question a filesystem can be asked.

**The synchronous half is closed.** Tier 3 answers `exit`, `getpid`, `now` and `stage` inside the
process's own worker, with no kernel to ask — that is the whole reason the same binary runs at
either tier. A fifth synchronous operation would have nothing to answer with there and would fail
at tier 3 alone, which is the worst way for an ABI to break. So an operation that needs the kernel
is asynchronous whatever it costs, and the four above are the complete set for good.

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
  `src/proc/` or `src/cmd/` depends on.
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
BundleFs   → one packed archive  (read-only /bin and /share — immutable, cheap)
OpfsFs     → OPFS                (read-write /home — the real store)
MemFs      → linear memory       (/, and the fallback when OPFS is absent)
ProcFs     → the scheduler       (/proc, generated at open)

unbuilt:
  a File System Access Fs        (a real local directory, Chromium only, opt-in — §5.4)
  an Fs over Range requests      (read-only remote trees)
```

Several of those differ from what this section first said:

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
- **`/bin` and `/share` are two views of the one archive.** M5's `/bin` was `BinFs`, a
  filesystem over the program registry, because programs were in-kernel coroutines and `/bin`
  would otherwise have been an empty directory `ls` could not account for. Now that every
  program is a binary the directory holds the binaries themselves, and `BinFs` is gone —
  which is the promise M5 made when it wrote that the mount would change and nothing above it
  would. `bundlefs_at` re-roots a bundle onto one of its subtrees, so the programs and the
  files that ship beside them stay one download and become two mounts. There is no `/usr`: one
  archive with two entry points does not need a third directory level to explain it.
- **`/proc` is `ProcFs`, the same trick over the scheduler** (M7): `cwd`, `meminfo`, `uptime`,
  `version`, `mounts`, `jobs`, and one file per live pid. It is also the reason the process ABI
  is as small as it is (§4.3): a process reads its answers here rather than asking for an
  operation of its own. `cat` and `grep` are then the
  introspection tools and there is no second interface to keep in step. The tree is flat —
  `/proc/42` is a file, not a directory — because a process here has one line of state, and a
  generated directory level would hold exactly one file. Content is produced at `open` and read
  out of that snapshot, so a two-block read cannot describe two different moments.

  `/proc/cwd` is the **kernel's** working directory — what init runs `/bin/sh` from — and every
  process's own is a line in its own `/proc/<pid>`. The shell's is one of those now rather than
  the first. `/proc/jobs` is gone with the same change: the job table is a process's memory, and
  no syscall shows one process another's.

**Every process has a working directory of its own**, inherited from whoever spawned it and moved
only by its own `chdir`. There is no longer a special one: the shell's is the shell process's,
`cd` moves that, and a typed command inherits it at spawn — which is also what a redirection on
that line is relative to, since the shell opens those itself before any stage runs. A `cd` in one
process moves nobody else's feet, and that is the whole of why `cd` is a builtin: a `/bin/cd`
would move its own and exit, leaving the shell where it was.

The kernel keeps one for itself, which is where init resolves `/bin/sh` from and what `/proc/cwd`
reports. A process is therefore isolated in address space, memory, descriptors *and* the directory
it names things from. What it is still not isolated in is the namespace itself: there is no
per-process root, and `open` resolves with the kernel's full authority once the path is absolute.

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

Its reach is limited (Appendix A), so treat it strictly as progressive enhancement, offered only
where `window.showDirectoryPicker` is defined. Directory handles are structured-cloneable, so we
can stash one in IndexedDB and re-offer the mount on the next visit — though permission must be
re-requested each session.

**This is unbuilt.** `mount` is an ordinary binary that reformats `/proc/mounts`, and mounting is
not yet something a user does: `vfs_mount` is called from boot and from nowhere else. Making it
one needs the `Fs` above, a syscall or a `/proc` write to reach it, and an answer to what a
second process should see — which is the same namespace question §4.3 leaves open.

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

As created in M0; `src/user` arrived with M3, `src/fs` with M5, `src/svc`
with M6, `src/ui` with M7, and `src/proc` and `src/cmd` with M8. `braam_fs` and `braam_svc` are
siblings above the kernel and below userland, depending on neither the other nor upwards.
`braam_ui` is in neither hierarchy: it is linked by `braam_proc` and *not by the kernel at all*,
because the programs that paint are binaries. `src/proc` is likewise a *different binary's*
runtime — what it shares with the kernel is four translation units and a handful of headers.

```
doc/Concept.md          this document
doc/Milestones.md       the plan: M0–M9 and their acceptance criteria
doc/Release_Notes.md    reasoning behind the code, milestone by milestone
doc/System_Calls.md     the kernel↔process mechanism, end to end (§4.3)
Makefile                wrapper: all, run, serve, release, clean
CMakeLists.txt          the build
cmake/                  the wasm32-unknown-unknown toolchain file
src/kernel/             allocator, core types, Task, scheduler, Channel, screen
src/kernel/coroutine.h  the freestanding <coroutine> shim (Appendix C)
src/kernel/hostcall.h   the asynchronous host request, shared by both interfaces
src/kernel/jsref.h      the externref table and JsRef (§3.7)
src/kernel/sysabi.h     the kernel↔process wire, included by both sides (§4.3)
src/proc/               a process binary's whole runtime: _start, syscalls, stdio
src/cmd/                one file per program; every program is a binary of its own
src/fs/                 Fs interface, path, VFS, MemFs, BundleFs, OpfsFs, storage ABI
src/svc/                fetch, WebSocket, clipboard, file transfer, clock, processes
src/ui/                 the layout layer over a Grid: Pane, TextBuf, TextView (§3.5)
src/user/               exec and the syscall dispatcher, the console and its pump, the
                        pipes behind a stage's stdio, ProcFs, boot and init
src/user/tty.h          the terminal claims: KeyInput, FullScreen
src/sh/                 the shell: grammar, LineEditor, job runtime, builtins
src/cmd/sh.cpp          its entry point — /bin/sh is a binary like any other
bundle/                 the tree tools/pack.py packs into /bin and /share
test/                   in-wasm unit tests, the Node driver, and the fakes: storage,
                        services, and a tier-3 worker with no thread in it
web/                    braam.js (the embedding API), worker.js, host shim, renderer
web/proc.js             both halves of the process protocol; procworker.js is a tier-3
                        process's worker, and wiring only
tools/                  build scripts, bundle packer, metadata stamper, version and
                        release scripts, size-budget check, chat server
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

A hand-written shim over the `__builtin_coro_*` intrinsics — declaring
`std::coroutine_traits`, `std::coroutine_handle<>`, `std::coroutine_handle<P>`,
`std::suspend_always` and `std::suspend_never` — compiles cleanly with (the file that grew
out of it, [src/kernel/coroutine.h](../src/kernel/coroutine.h), is 124 lines with its comments):

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

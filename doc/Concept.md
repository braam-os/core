# Braam — Concept

An interactive, CLI-oriented operating system that runs entirely inside a
browser tab, written from scratch in freestanding C++20 and compiled to
WebAssembly.

This document is the specification: what the system is and what its parts must
do. It changes only when a design decision changes, in the same commit as the
code. Its section numbers are cited from source comments — amend a section, do
not renumber it.

The other documents are subordinate to this one.
[Release_Notes.md](Release_Notes.md) says *why* the code is the way it is, and
holds the milestones M0–M9 with the criteria they were accepted against.
[System_Calls.md](System_Calls.md) walks §4.3's kernel↔process mechanism end to
end, with the operation table in full. [Shell.md](Shell.md) is the manual for
`/bin/sh` and §4.5's language.
[Programming_Manual.md](Programming_Manual.md) is the SDK's guide.
[Package_Management.md](Package_Management.md) is the policy a package manager
must satisfy: what a package must prove, and how the signing keys are held.

---

## 1. Goal

A kernel, a shell, a filesystem, a terminal and a set of programs, reached by
opening a URL and deployable as a **static site** — no server, no build-time
secrets, no special HTTP headers.

That constraint drives the design:

- **No `SharedArrayBuffer`**, therefore no `COOP`/`COEP` headers, therefore any
  dumb static host serves it as-is.
- **No Asyncify**, no JSPI, no stack-switching machinery of any kind.
- **No Emscripten runtime.** Nothing is linked that we did not write.

Non-goals, chosen deliberately:

- **POSIX compatibility.** No `open`/`read`/`write`/`fork`, and no aim to run
  third-party C.
- **A VT100 emulator.** No ANSI escape parsing, no `xterm.js`.
- **A general-purpose libc.** Exactly the foundation our own code needs.

Dropping POSIX is the highest-leverage decision in the project: it costs the
ability to drop in existing C programs and buys a system an order of magnitude
smaller, with no emulation layer, no escape-sequence parser to be attacked
through, and every mechanism native to the browser.

---

## 2. Organizing principles

Three invariants hold the design together, and nearly every "how should X work?"
is answered by one of them.

### 2.1 Coroutines are processes; the event loop is the scheduler

C++20 coroutines *are* the process abstraction and the browser event loop *is*
the scheduler. Everything that would block becomes a `co_await`. Nothing blocks,
so nothing needs a stack of its own: a suspended process is a coroutine frame in
a hash map, costing one allocation.

### 2.2 An import never returns data — only accepts a token

Every JS import is non-blocking and returns immediately. It accepts a *wake
token*; the result arrives later through the `wake()` export. This keeps the
boundary uniform and makes any new asynchronous browser API a ~20-line change on
each side.

**Two exceptions are sanctioned**, both because no promise is involved at any
point:

1. `host_now()` — a clock read.
2. **OPFS sync access handles** — once a file is open,
   `read`/`write`/`getSize`/`truncate`/ `flush` are genuinely synchronous
   (§5.2).

A third needs a written justification in this document. One or two pragmatic
exceptions are fine; three are a second calling convention, and then there are
two ABIs and no invariant.

Calls in the other direction are not exceptions to this rule, because they are
*exports*: `ref(slot, obj)` (§3.7) stores a JS object and returns, and
`sys`/`sys_async` (§4.3) are a process's imports that the host forwards.

### 2.3 The terminal is a cell grid, not a byte stream

The kernel owns a buffer of cells in linear memory and the renderer draws it.
There is no stream of bytes carrying control codes, because there are no control
codes.

Colours and styling are struct fields, cursor addressing is array indexing, a
`curses`-style layout layer is trivial rather than a parser, and there is no
escape sequence to mis-parse. The whole renderer is ~300 lines of JavaScript.

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

The kernel runs in a Web Worker and communicates by plain `postMessage`.
Rendering happens against an `OffscreenCanvas` transferred into that worker, so
the main thread stays free, and a "reset kernel" button is `worker.terminate()`
followed by a reboot. A runaway program hangs neither: it is a worker of its own
(§4.2), and the kernel is merely waiting for a reply it can stop waiting for.

### 3.1 Toolchain and language subset

Target `wasm32-unknown-unknown`, freestanding. Any clang with the wasm32 target
and `wasm-ld` will do, because it is used purely as a compiler: none of its
runtime and none of its headers are linked or included. Homebrew's `llvm` and
`lld` are the local default and Debian's `clang`, `lld` and `llvm` are what CI
installs; nothing is pinned.

```
clang++ --no-default-config --target=wasm32-unknown-unknown -std=gnu++20 -Os \
    -nostdlib -nostdinc++ -fno-exceptions -fno-rtti -fno-threadsafe-statics \
    -mreference-types -mbulk-memory -msign-ext -mmutable-globals -mnontrapping-fptoint \
    -ffunction-sections -fdata-sections \
    -Wl,--no-entry -Wl,--gc-sections -Wl,--stack-first -Wl,-z,stack-size=131072
```

Appendix C explains each flag, and why `--export-dynamic` and
`--allow-undefined` are deliberately absent: adding either back is a regression.
libc++'s `<coroutine>` cannot be used freestanding, so
[src/kernel/coroutine.h](../src/kernel/coroutine.h) is a shim over the
`__builtin_coro_*` intrinsics.

**No exceptions, no RTTI.** Errors are values: `Result<T, E>`, propagated
through `co_await` with a `TRY()` macro rather than by unwinding.

### 3.2 The foundation we own

About 1,500 lines that we are glad to control:

- **Allocator** — a bump arena plus size-class free lists over `memory.grow`.
  Coroutine frames go through it, so it must be fast (§8.2).
- **Core types** — `Str` (a UTF-8 view), `String`, `Vec<T>`, `Span<T>`,
  `Result<T, E>`, `Option<T>`, `HashMap<K, V>`.

Nothing else is available: there is no libc, `new` is not used
(`heap_new`/`heap_delete` are), and a namespace-scope global must be trivially
destructible, since a non-trivial destructor needs `__cxa_atexit`. State that
needs a constructor lives behind a pointer built on first use, as `Sched` and
the process runtime's `Rt` do.

### 3.3 The core abstraction

```cpp
template <class T> struct Task {           // lazy, movable, awaitable
    struct promise_type { ... };           // symmetric transfer on final_suspend
};

struct Waiter {                            // a suspension record, in the frame
    std::coroutine_handle<> h;
    CancelState *cancel;
    u32 token;                             // handed to JS, comes back later
    u32 payload_ptr, payload_len;          // what wake() delivered
    ...                                    // and which queue it is registered in
};

struct Wake { ... };                       // co_await: until wake(token) arrives
struct Sleep { ... };                      // co_await: until a deadline passes
```

The scheduler is a ready queue of `std::coroutine_handle<>`, a timer queue, and
a `HashMap<u32, Waiter *>` of suspended tasks keyed by wake token. The `Waiter`
lives in the suspended coroutine's own frame, so registering costs no allocation
and `wake()` has somewhere to put its payload. A `Wake` takes its token in its
*constructor*, before the `co_await`, which is how an async import can tell the
host "notify me on token N" and only then suspend; `await_suspend` registers the
`Waiter` and the destructor deregisters it (§8.1). A token alone cannot say
what it is for, so the `Waiter` carries that too (§3.6). `tick()` is the only
thing that ever resumes a coroutine.

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

The last two are not the host's own business: they are an isolated process's two
imports, which the host forwards with the pid it bound into that process's
closure. A process therefore cannot name another — there is no argument for it
on its side of the call. Both are entered from JS at top level, never from
inside a kernel import, which is what keeps them as ordinary as `key()`.

`resize` returns where the screen descriptor (§3.5) lives, which is how the host
learns the geometry and the address of the cell array. It is the only call that
moves the cells, so it is where the renderer re-derives its views (§8.4). The
kernel clamps the geometry it is given, so the host reads `cols` and `rows` back
out of the descriptor rather than assuming its request was honoured.

**Wasm imports** (kernel → host), all non-blocking:

```
host_now(), host_log(ptr, len)
host_present(dirty_x, dirty_y, dirty_w, dirty_h)
host_fs(op, token, req)                        // storage, async  (§5.2)
host_fs_sync(op, handle, ptr, len, off) -> i32 // storage, sync   (§5.2)
host_svc(op, token, req, ref)                  // host services, async (§6)
```

Six, and the smoke test asserts exactly these.

Storage and services are **multiplexed rather than named per operation**: one
import per *calling convention*, so a new operation is an enum value on each
side rather than a new import, and the exact-import assertion stays stable while
operations are added. `req` is the address of a `HostRequest`
(`src/kernel/hostcall.h`) carrying the string argument, the flags, a reply
buffer, an `aux` word and the status. Both asynchronous interfaces share that
record, one orphan list and one reaper. **The kernel owns the record for as long
as the host may touch it**, which is past a cancelled await, so it outlives its
awaiter rather than being freed under the host.

There is no `host_timer`: the kernel owns the timer queue, so `tick()`'s return
value says when the host must call back and one `setTimeout` serves every
sleeping task. Compiling, instantiating and stepping a process are `host_svc`
operations rather than imports of their own.

### 3.5 The screen and the keyboard

```cpp
struct Cell { char32_t ch; u8 fg, bg, attrs, reserved; };   // 8 bytes; fg and bg are palette indices
```

The renderer holds a view over the cell array and blits monospace glyphs to the
canvas, plus a cursor. Damage tracking is a dirty rectangle the kernel updates
as it writes, passed to `host_present` once per `tick`. The cursor is drawn,
never stored, so moving it dirties the cell it left as well as the one it
entered. The host finds all of it through a descriptor, whose address `resize`
returns:

```cpp
struct Screen {
    u32 magic;                 // 'BSCR', so a mismatched renderer fails loudly (§8.4)
    u32 cols, rows;
    u32 cursor_x, cursor_y;    // cursor_x may equal cols: the wrap is deferred
    u32 cursor_on;
    u32 cells;                 // address of Cell[cols * rows]
};
```

The wrap is deferred, so filling the last column does not scroll the screen on
its own. A resize keeps the rows in use — `0..cursor_y` — dropping from the top
when they no longer fit, and lands them at the top of the new grid. Re-wrapping
logical lines needs a per-row continuation bit the grid does not have;
scrollback arrived without it, and it is still outstanding.

**A row that leaves the top is kept**, in a ring of `SCREEN_SCROLLBACK` rows at
the grid's own width, fed by the scroll and by the resize's drop and by nothing
else. **Shift+PageUp and Shift+PageDown page a view over it**, half a screen at
a time, **and Shift+Up and Shift+Down move it a row**, which is what a wheel
notch arrives as; any other key returns to the live screen. The console pump
owns the chord, since the history is the kernel's grid — but not while a program
holds the screen, whose grid it is not, and where the same keys reach the
claimant instead.

The renderer is told nothing. While a view is up the live grid is left exactly
where it is, so output, the cursor and `screen_scrolled()` carry on into it
unaware, and `cells` points at a composed block instead: the rows above the live
screen out of the ring, the rest out of the grid. So the descriptor's `cells`
moves when a view opens or closes, the cursor is hidden rather than turned off,
and a selection over scrollback is the same arithmetic over the same array. The
composition is done once per `tick`, not once per scrolled row, and the view
stays on the rows being read as output moves the grid underneath — until the
ring's oldest row is evicted, past which it drifts, because those rows are
genuinely gone.

**The layout layer over the grid** is `src/ui/`, four small things rather than a
widget toolkit:

- **`Grid`** — cells, a cursor and a damage rectangle, and nothing else. The
  kernel's screen is one; a full-screen program paints another of its own, in
  its own address space, and blits the damaged part across with one syscall
  (§4.3).
- **`Pane`** — a rectangle with its own coordinates, style and cursor. Every
  write is clipped to it, so a status line cannot scribble on the text above it.
  It never scrolls, because scrolling moves the whole grid.
- **`TextBuf` and `TextView`** — logical lines and a window onto them. `less`
  and `edit` differ in what they do with keys, not in how they scroll.

`src/ui/` is a library a *process binary* links; the kernel does not link it at
all. The alternate screen is the one piece that stays kernel-side, as
`FullScreen` in `src/user/tty.h`: it copies the grid to a heap block, blanks it,
and copies it back in its destructor, which is what gives the shell's screen
back when a program is killed — a killed process runs no destructor of its own.

**Input is symmetric.** A normalised `KeyboardEvent` becomes `{code, mods}`, is
posted to the worker, and lands in a `Channel<Key>`. A printable key carries its
Unicode codepoint; named keys take values above the Unicode range. **No control
characters exist anywhere in the system**: `^C` is `'c'` with the control
modifier set, and the reader decides what that means. That is §2.3 applied to
input. Line editing — history, cursor movement, kill-word, completion — is a
userland `LineEditor` coroutine, not a termios state machine in the kernel.

**The focus is not on the canvas.** A software keyboard is raised by a focused
*editable* element, and a canvas is focusable but not editable — so the page
holds one, a hidden `<textarea>` that `web/braam.js` creates and never shows,
and that is where every key event arrives. A canvas that took the focus would be
a terminal no tablet could type into. There are therefore **two sources and one
destination**: a `KeyboardEvent` through `normalise()`, and — for a keyboard
that reports no key, which is what a soft keyboard, dictation and every IME do —
the text an `input` or composition event produced, turned into key codes the way
a paste is. One rule decides between them: **the text route runs exactly when
the key route did not prevent the default.** Both end as the same
`{code, mods}`, and nothing below the page can tell which one a keystroke came
from.

**One receiver on that channel, and it is the console pump**
(`src/user/console.h`), which init spawns and which never ends: something must
hold the keyboard while nothing is running, and a process has no `keys()` at
all. A program therefore does not take the keyboard — it **claims a route
through the pump**, and the prompt is no exception.

**Each of the two routes — raw keys and the screen — has one holder at a time,
on the kernel, named by the pid that took it.** A second claim is `Err(Perm)`
rather than a nested one, and a claim clears its route only if it is still the
holder, so a parent and a child may die in either order. Nesting would mean
restoring a predecessor that may already be gone. Painting is held to the same
rule: a `ScreenBlit` from a process that does not hold the screen is refused
(§4.3).

**`^C` cancels the foreground if there is one, and is delivered to the claimant
if there is not.** The foreground is a set of pids a process arms with
`Sys::Fg`, which is what a shell does for each stage of a pipeline before it
waits; the pump cancels them, so a program that has taken the screen and stopped
answering stays killable. With nobody in front the interrupt is an ordinary key
going to whoever holds the raw route — which is what lets a line editor abandon
the line being typed instead of being cancelled by it. Without that split, a
shell that is a process would be killed by its own `^C`.

Everything the pump does not route to a claimant it **cooks**: echo, a line at a
time, `^D` for end of input, into one console channel that is the stdin of
whatever is in front. A shell hands that channel to a child by letting go of the
keyboard, which is why `cat` with no argument reads what is typed.

**Selecting and copying are the page's business, and the kernel is told
nothing.** A drag over the canvas never reaches wasm: `web/braam.js` turns it
into device pixels, `web/render.js` turns those into cells and reverses them as
it reverses the cursor, and the text it reads back out of the grid crosses to
the page when the drag settles. There is no mouse event in the ABI, no selection
in the `Screen` descriptor and nothing a program can ask, because a selection is
a *view* over the grid rather than input and the grid is already shared (§2.3).
`Ctrl+C` — `Cmd+C` on a Mac — copies when there is a selection and is `^C` when
there is not; copying clears the selection, so the next one interrupts. The
clipboard write happens inside the keydown handler, because that keystroke is
the transient activation permitting it (§A.2). Any other keystroke, and any
resize, drops the selection. Select all is `Cmd+A`, or `Ctrl+Shift+A` where
there is no `Cmd`, and deliberately not `Ctrl+A`, which is the line editor's
beginning-of-line. The focus deciding which terminal on a shared page owns the
copy chord and the paste event is the hidden input's rather than the canvas's;
the canvas carries a `braam-focus` class so a page can still draw a ring around
it.

**The wheel is page-side in the same sense, and becomes the scrollback chord.**
A `wheel` over the canvas is turned into the keystrokes above — one Shift+Up or
Shift+Down per row, the half-screen chord for a page-mode delta — so the history
is scrolled by the only mechanism that reaches it and the ABI still has no mouse
in it. The delta crosses to the worker in device pixels, as a drag does, because
the worker owns the font and therefore the row height; the fraction of a row
left over is carried, or a trackpad's small deltas would never reach one. The
run is fed through `key()` like a paste and takes the ring's back-pressure the
same way. `Ctrl`+wheel is the browser's zoom and is left alone; everything else
over the canvas is the terminal's.

**The key bar is page-side in exactly the sense the selection is.** A software
keyboard has no `Ctrl` and no `Esc`, and usually no `Tab` and no arrows, so a
page may hand `mount()` a container and get a row of buttons for them, `Ctrl`
latching onto the next key sent. What comes out is an ordinary `{code, mods}`:
there is no bar, no latch and no touch event anywhere in the ABI, and the kernel
cannot tell a tapped `Esc` from a typed one. The page supplies the keys the
hardware does not have — not a second kind of input.

**A paste is a run of keystrokes, and nothing downstream can tell it from fast
typing.** There is no byte stream to write into (§2.3), so `web/keys.js` turns
the pasted text into key codes — one `Enter` per newline, `Tab` for a tab,
nothing for a control character no key produces — and the worker feeds them
through `key()`. That is why a paste needs no import, export or syscall of its
own. What a run does need is **back-pressure**, and that is the whole reason
`key()` returns something: the ring holds 64 keystrokes, so the host feeds a
paste at the rate the console drains it rather than losing the tail. That return
value reports a fact the host cannot otherwise observe; it is not an answer
arriving from the kernel, so §2.2 is untouched.

**Soft-keyboard, dictation and IME text take that same road**, and for the same
two reasons: the ring's back-pressure, since a dictated sentence arrives all at
once, and ordering — a `key()` is dispatched ahead of a run still being fed,
deliberately, so that `^C` never waits behind a paste, which means a backspace
posted as a key could overtake the word it follows. So everything the text route
produces, backspace and `Enter` included, is fed as a run. The one exception is
the character after a latched `Ctrl`, which wants to jump that queue precisely
because it is `^C`.

`Cmd+V`, or `Ctrl+V` where that is the chord, is the browser's own gesture and
is not prevented, precisely so the `paste` event is produced. That event is the
document's rather than the canvas's, so a terminal claims one only while it
holds the focus. Within the terminal that has the focus, a `pbpaste` waiting for
the same gesture (§6) takes the text and nothing is typed.

### 3.6 Kernel objects

- **`Channel<T>`** — an async MPSC queue with bounded capacity:
  `co_await ch.recv()` and `co_await ch.send(v)`. This one type is the pipe, the
  stdin and the IPC. It has **one receiver**, and panics on a second blocked
  sender rather than losing a wakeup quietly.
- **A scheduler job** — a `Task<i32>`, a name, a `CancelToken` and the time it
  was spawned, which is all the scheduler keeps. `sched_spawn()` pushes one on
  and hands back its pid; killing means signalling the token, and every
  `co_await` point checks it and unwinds by returning, so destructors run.
  `sched_procs()` reads the table back out, which is what `/proc` is made of
  (§5.1) — including what a suspended job is waiting on, which the awaiter
  records in its `Waiter` because a wake token alone cannot say whether it
  belongs to a channel or a host call. There is no `Process` type: argv and
  stdio belong to the pipeline stage (`src/user/prog.h`), and a working
  directory, a parent and a worker to the process record `exec` keeps.

  **Cancellation does not propagate down a tree.** `CancelState::waiting` is a
  single slot, so a job cannot have two children parked at once — which a
  pipeline needs, since its stages run at the same time. So a pipeline's stages
  are independent jobs and §3.6's parent-child relationship is put back by hand:
  `run_line`'s frame holds a destructor that cancels every stage it started, on
  its way out for any reason. The cost is that a cancelled child does not unwind
  until the scheduler resumes it, a tick or two after its parent is gone, so it
  must touch nothing the parent owns. A real child-group awaitable needs
  intrusive queue links inside `Waiter` first.
- **Filesystem** — an async node tree, not inodes. One interface, split by
  *when* the work can happen rather than by what it does: naming a file may need
  the host and therefore a wake token, but an already-open file does not (§5.2).

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
      virtual Task<Result<void>>       touch(Str path);   // Unsupported by default
      virtual Task<Result<void>>       symlink(Str target, Str path);
      virtual Task<Result<String>>     readlink(Str path);
      virtual Task<Result<void>>       rename(Str from, Str to);

      virtual Result<usize> read(u32 h, u64 off, u8 *buf, usize n);
      virtual Result<usize> write(u32 h, u64 off, const u8 *buf, usize n);
      virtual Result<u64>   size(u32 h);
      virtual Result<void>  truncate(u32 h, u64 n);
      virtual void          close(u32 h);
  };
  ```

  `read` fills a caller's buffer rather than returning a `Bytes`, because
  nothing below this line owns a buffer the caller can keep. An implementation
  sees paths already resolved and relative to its own mount point, so it never
  has to know where it was mounted. A mount table maps prefix → `Fs`, longest
  prefix winning, and an open-file table above it holds the descriptors.
  Implementations in §5.1.

  A `Stat` is `{kind, size, mtime}` and a `NodeKind` is `File`, `Dir` or `Link`
  — three, and no inode, mode, owner or link count. **A store reports a link
  and never resolves one**: following is `vfs_resolve`'s, above the mount table,
  because a target may name a path in a different filesystem and nothing below
  that line can see the table. It is also what lets a listing stay a listing —
  `ls -R` and the globber descend on `Dir` alone, so a walk cannot follow a link
  out of the tree it is walking (§5.2).
- **Programs are not kernel objects.** One file in `src/cmd/`, built into a
  binary of its own (§4). The job table is not one either: it is the shell
  process's own memory.

### 3.7 Holding JS objects

A `Response`, a `FileSystemFileHandle` or a `WebSocket` is held in an
**`externref` table** as a slot index, with no serialisation, wrapped in an RAII
`JsRef` that frees the slot in its destructor.

**The table is the kernel's, and JS never indexes it.**
`import_module`/`import_name` apply to functions only, so a table cannot be
imported; a module-defined one is what the toolchain supports. The traffic
therefore runs this way round:

- The kernel reserves a slot and publishes the number in the request record.
- The host, when its promise resolves, calls the `ref(slot, obj)` export to
  deposit the object.
- To *use* it, the kernel reads the slot and passes the object as `host_svc`'s
  fourth argument. JS sees the object, never the table.

An instance's table is part of the instance, so a process can only reach the
objects its own kernel put there. A slot is owned: `JsRef` is move-only, and a
request that reserved one owns it until `await_resume` hands it over, so a
cancelled request frees the slot along with the record. A slot the host deposits
into belongs to the record rather than to a frame (`reserve_ref()`). Releasing a
*service* object additionally tells the host to let go — a socket has event
handlers holding it alive on the JS side — which is what `JsHandle` in
`src/svc/svc.h` adds.

OPFS handles are not in this table: a filesystem handle is only ever an integer
on the wasm side, so it is a plain JS array indexed by slot number.

---

## 4. Process model

**Every program is a binary in its own instance, in a worker of its own.** There
is no in-kernel program, no program registry and no way to write one. A program
gets its own address space, its own capabilities, its own descriptors and a
memory cap the kernel sets, inside a Web Worker holding nothing else — so
`worker.terminate()` ends it without its cooperation. There is nowhere else to
put a process: `braam_add_program` arranges it unasked, and the binary's `braam`
section carries a memory cap and an ABI number but no placement flag (§4.3).

**A host with no worker to give is waited out, not worked around.** Where the
constructor throws — a browser without nested workers, a host disposing of its
pool — the spawn is refused with `Error::Again` and `spawn_process`
(`src/user/exec.cpp`) backs off 10, 20, 50, 100, 200, 500 ms and then a second
indefinitely, printing `no worker, retrying` on the program's own stderr. It is
an ordinary await, so `^C` abandons it. Nothing is latched, so a host that
recovers is noticed. The alternative — instantiating in the kernel's worker — is
a process with no kill switch sharing the kernel's liveness, and a browser that
cannot make a nested worker cannot run Braam.

**The shell is not an exception.** `/bin/sh` is a binary in `/bin` that init
runs, and everything a prompt needs — a pipeline, a redirection, a job, a
working directory, the keyboard, the cursor — it asks for through §4.3. What is
left inside the kernel is not a weaker kind of process: it is the dispatcher
those requests arrive at.

**A process that loses its worker dies with it, and init replaces the shell.**
There is no moving a running process; the instance went with the worker. Init
starts another `/bin/sh` when its shell **died** — a trap, a failed step, an
instance that would not be made — and not when it **exited**, which is the
user's own `exit` and the end of the session. It is bounded at three deaths in
quick succession; a shell *waiting* for a worker is not one, since it has not
started. A replaced shell is a fresh one: kernel `/home`, empty job table.

**A shell builtin is not kernel code**: the twenty-six of them live inside
`/bin/sh`, in `src/cmd/sh/builtin/`. Two clauses make one. The first is that it
touches the shell *process's own* state — its working directory, which a typed
command inherits at spawn; its job table, which no syscall shows anyone; its
variables, its options, its traps, its loop. That is `cd`, `fg`, `jobs`, `kill`,
`exit`, `set`, `shift`, `read`, `trap`, `wait`, `export`, `readonly`, `unset`,
`break`, `continue`, `return`, `.`, `eval`, `exec` and `command`, whose answer
*is* that state — the function table and the builtin table, which no syscall
shows anyone either. **`help` is not one of them**: what a command is for is a
document rather than state, so it is `/etc/help` and the `#!` script that pages
it, and the table carries no usage string to go stale. The second is that **its
whole cost is the spawn**: a program costs an instantiation and a worker,
roughly a millisecond (§4.4), and `while [ … ]; do echo …; done` pays it twice a
turn, so `test`, `[`, `:`, `echo`, `true` and `false` are builtins too — a few
lines each, where the spawn *is* the runtime. The clause is closed and admits
nothing else.

**A builtin of the second kind keeps its file in `/bin`.** The shadowing is at a
prompt, not everywhere: `/bin/test` is what a future `find -exec` would run, and
there is nowhere else to put it. One of the first kind has no file and never
will — `rm /bin/cd` finds nothing.

**A shell function is looked up first**, ahead of both: it is the same rule
named by the user, and it runs in the shell's own turn for the same reason a
builtin does. So a command word resolves as **function, then builtin, then
`PATH`**, and only the last of the three costs a process.

**`PATH` is the kernel's, not the shell's.** The search is in `exec_resolve`,
which reads the one word out of the environment the spawn carries (§4.3), so it
steers every spawn there is and not only a typed command: `timeout ls`,
`watch ls`, `PATH=/x ls` and a script's own children all search the same list.
A shell that resolved the word itself would leave the other three looking
somewhere else, and a `PATH` that did not steer resolution would be a lie a
script could believe. A word with a `/` in it is a path and is never searched;
an environment naming no `PATH` searches `SYS_PATH_DEFAULT`, which is
`/bin:/pkg/bin`.

**There is no fourth clause, and installed software does not get one.** A
package manager reaches its programs the way anything else does: by putting a
directory on `PATH`. The design is in
[Package_Management.md](Package_Management.md) — a generation of installed
packages is materialised as a directory of symbolic links into `/pkg/store/`,
`/pkg/bin` names the live one, and the default search list is `/bin:/pkg/bin`
with `/bin` first, so nothing installed can shadow the system. That list is
**built**, and it is the whole of the kernel's part: `exec_resolve` gained no
clause, and a missing `/pkg`, a dangling `/pkg/active` and a link into nothing
are each an ordinary component that finds nothing. What fills the tree is
`/bin/pkg`, and it too is built. The alternative was a clause in `exec_resolve`
after the `PATH` search, reading `/pkg`'s own record of which generation is
active and which command it names — two file formats the
kernel would have to learn, two reads on every failed lookup, and four ways of
being half-installed that each had to come back as an ordinary "command not
found" rather than a boot that would not finish. Symbolic links, `PATH` and a
rename buy the same activation with no kernel code at all, and a rename is
already the commit point a generation needs.

Either way a builtin pipes and redirects through descriptors like anything else,
but runs **in its turn rather than alongside**, since nothing inside a process
can wait for a sibling task. So **a builtin buffers its output and writes it
once**: one that wrote a line at a time would fill an eight-slot pipe and park
with nobody left to drain it.

### 4.1 What separate instances buy

- **Address space: isolated, for free.** Two instances have two
  `WebAssembly.Memory` objects, and no instruction reaches outside your own
  linear memory. A wasm pointer is an offset, not an address; there is nothing
  to forge. This is stronger than MMU-based isolation, because it is enforced by
  the type system and bounds checks rather than by page tables one might
  misconfigure.
- **Capabilities: isolated, if we are careful.** An instance can only call the
  imports supplied at instantiation. Each closure is bound to its pid, so
  process 7 physically cannot issue a syscall as process 3: it holds no function
  that does so. The same applies to the `externref` table (§3.7).
- **Memory limits: isolated, and a bonus.**
  `new WebAssembly.Memory({initial, maximum})` is a hard ceiling — 256 pages, 16
  MB — and `memory.grow` simply fails past it. That is an rlimit without
  cgroups. When a process ends the instance is dropped and *all* its memory
  returns at once.

### 4.2 What they do not buy: CPU time

**`while(1){}` cannot be preempted.** Nothing in the wasm specification allows
it, so address-space isolation and *liveness* isolation are separate problems.
One worker per process makes `worker.terminate()` the `SIGKILL`, which is what
an operating system owes its user, and it needs no metering: a step is one more
asynchronous host request, so a process that never answers is a request that
never lands, and `^C`, `kill` and a cancelled job already know what to do with
one of those. **Fuel counters** — a binary-rewriting pass injecting
`if (--fuel < 0) trap;` at loop headers — remain the only way to *bound* CPU
rather than end it, and are unbuilt.

Workers are hired from a small pool, sized for a pipeline *above* what the
session holds permanently, since the shell is one of these processes and never
gives its worker back. A worker that has finished its process is clean — the
instance is dropped and wasm cannot have touched the worker's own scope — so it
goes back to the pool. One that was terminated is gone, which is the point.

### 4.3 The kernel↔process ABI

`src/kernel/sysabi.h` is the wire, included by both ends so neither can drift
alone. The `abi` word in the custom section is what makes an amendment safe:
`exec` refuses a binary whose number is not the kernel's, so a stale binary is a
diagnostic rather than a wrong answer.

**Two kinds of file are executable, and the second is defined by its first
line.** `exec` reads the image and looks for `\0asm` and the `braam` section;
failing that, a file beginning `#!` followed by an absolute path names an
interpreter, and what is instantiated is *that* binary, entered with the
interpreter, its one argument if the line carried one, the resolved path of the
script, and then the caller's own arguments. Three bounds make it a rule rather
than a search: the lookup is **one level deep**, so an interpreter that is
itself a script is `Err(Invalid)`; the interpreter must be **absolute**, since
`PATH` is the caller's and a file that named its interpreter by a bare word
would run a different one depending on who ran the script; and the first line
must end within `PROC_SHEBANG_MAX` bytes, so a file is decided by its head. A
script costs **one** process, not two — the resolution happens in `exec_resolve`
before any process exists, so the depth and child caps count the interpreter as
they counted a binary, and every script shares the interpreter's
already-compiled `Module`. The kernel does read file contents to decide what a
program is; it always did, for the custom section. What it does not do is guess,
and a text file with no `#!` is still `Err(Invalid)`. An interpreter that is not
there is `Err(Invalid)` too, rather than the `Err(NotFound)` of a command that
does not exist: 126 and not 127, because the file is there and it is the file
that will not run. A search says the same thing the same way: a file on `PATH`
that is neither kind does not shadow the binary behind it — there are no
permissions, so being a program is the only executability test there is — and a
search that found nothing else is `Err(Invalid)` rather than `Err(NotFound)`.
`test -x` answers from the same two rules, out of one `SYS_CHUNK`, and is what
`command -v` walks `PATH` with.

```
process imports:  env.memory                        // the kernel's, so the cap is the kernel's
                  sys(op, a0, a1, a2) -> i32        // sync ops, immediate result
                  sys_async(op, token, ptr, len)    // async ops, reply via _resume

process exports:  _start(ptr, len) -> i32          // argv then env; 0 = exited, 1 = suspended
                  _resume(token, ptr, len)   -> i32 // the same
                  _alloc(n) -> ptr, _free(ptr, n)

custom section "braam":  magic, abi, flags, initial_pages, max_pages
```

The coroutine model survives the boundary intact: the process's `co_await`
suspends, its scheduler returns control out through `_start`/`_resume`, the
kernel continues, and later calls `_resume` with the payload. Reentrant
scheduling across an instance boundary, with no stack switching.

The wire's conventions:

- **Memory is imported rather than exported**, with no declared maximum, so
  §4.1's ceiling is the kernel's decision and not a number the binary could have
  written differently.
- **`_start` takes argv rather than argc**, because the host places the blob
  through `_alloc` and `argc` alone cannot say where it put it. The blob is
  `u32 argc`, then a length and bytes per word.
- **The environment follows argv in that same block, in that same encoding**,
  its words `NAME=value`. One codec rather than two, and the join is found by
  walking the first blob (`argv_bytes`) rather than by a length word, so
  `argv_count` and `argv_at` read the payload unchanged. The block is never
  freed, so both are spans of views into it and a program may hold them to the
  end — which is why the runtime walks the environment on demand rather than
  building a `Vec` every process would pay for. **The kernel reads exactly one
  word of it**, `PATH`, and only to resolve the command name; every other word
  is the caller's business, as an argv word is.
- **A reply payload begins with an `i32` status**: `_resume`'s signature has
  room for a buffer and not for an errno, and every asynchronous syscall needs
  both.
- **The op word's upper bits are the operation's argument** — a descriptor, the
  open flags, or one small immediate — so a payload is only ever the operation's
  *data*, with no header glued on the front.
- **A process may have several syscalls outstanding** — one per task, and
  `PROC_TASKS` is eight — and the step request's `flags` says which one a reply
  answers. Kernel-side each parked call is a record with its own staging block
  and its own scheduler job, so a socket read that never completes cannot starve
  the keystroke behind it.

**The table is forty-four operations and `PROC_ABI` is 16**: four synchronous —
`exit`, `getpid`, `now`, `stage` — and forty asynchronous. `Verify` and
`Inflate` are the newest, and are what `/bin/pkg` needs of the host (§6): a
signature checked before an index is believed, and a way to read a zip that is
not the boot archive. [System_Calls.md](System_Calls.md) lists them all with
what each carries.

Four rules bound the table:

- **Every operation has a caller in `src/cmd/`.** That is a rule against
  *growing* the table on speculation, not one that retires an operation whose
  caller is refactored. `Dup`'s caller is `/bin/sh`, which is a program like any
  other.
- **The synchronous half is closed at four.** Each is answerable inside the
  process's own worker with no kernel to ask — `getpid` from the closure, `now`
  from the step message's clock plus elapsed time, `exit` buffered onto the
  step's reply, `stage` refused with the "no room" answer the runtime already
  handles — which is the whole reason one binary runs there at all. A fifth
  would have nothing to answer with and would fail in a worker alone. So an
  operation that needs the kernel is asynchronous whatever it costs, including a
  `wait` on a child that has already exited.
- **A stream of bytes comes back as a descriptor**, so `read`, `write` and
  `close` serve it and nothing is duplicated: a fetched body is read like a
  file, a socket is written like one, and a killed process drops all of them
  with its handle table.
- **What the kernel publishes as text needs no operation.** `/proc` is a
  filesystem, so `mount` is `/proc/mounts` and `cat` and `grep` are the
  introspection tools. `pwd` is the one thing that argument cannot reach —
  ProcFs generates a file at `open` and has no idea who is reading — so `chdir`
  is an operation, because "which process is asking" is not a question a
  filesystem can be asked.

**A descriptor named in a spawn is moved, not duplicated.** The parent's slot is
closed and the child owns it. POSIX duplicates and expects the parent to close
its copy, and forgetting is the classic bug where the reader never sees end of
input. Moving makes it unrepresentable — and a `Channel` has one receiver and
panics on a second blocked sender (§3.6), so two processes holding one pipe end
would be a user program reaching a kernel invariant. A descriptor a syscall of
the parent is parked on cannot be moved at all, and a spawn refused on any slot
takes none of them. Within a process, a second concurrent use in the same
direction is `Err(Perm)`.

**A spawn carries an environment, or says nothing and the child inherits one.**
`Sys::Spawn`'s op-word argument has a bit for "an env blob follows the argv
one"; without it the kernel hands the child the caller's, which it already holds
on the `Proc` record beside the cwd. So the common case puts nothing on the
wire, `timeout 5 env` inherits without doing anything, and a shell that wants to
say exactly what a stage gets says it. A process's environment is **fixed at
spawn**: there is no `setenv`, because nothing would call one — a shell keeps
its own variable table and builds the blob afresh at each spawn. It is bounded
at `SYS_ENV_MAX`, since a child hands its own on and an unbounded one would grow
down a chain of them.

**A child is an ordinary scheduler job**, spawned exactly as a pipeline stage
is, so `^C`, `kill`, `jobs` and `/proc` reach it with nothing added. Its
parent's destructor cancels it, which is §3.6's structured concurrency put back
by hand one level further down; its status is recorded on the parent's record by
a destructor that finds the parent by pid. Both bounds — `SYS_CHILD_MAX` live
children, `SYS_PROC_DEPTH` levels deep — hold because every child is an instance
with a memory cap of its own, and nothing else would stop the first fork bomb.

**A pid is reused, but never while something still names it.** The system is
meant to run for as long as the tab is open, so a counter that only climbs is a
lifetime: `SYS_PID_MAX` is 999999 and both spaces wrap. Reuse is safe because
the scheduler skips a pid that is live *and* one that is reserved —
`sched_pid_hold`, taken by whatever outlives the job it names, which is an
uncollected `Child` entry and a foreground entry and nothing else. Everything
else that holds a pid holds it only while the job runs, or identifies its
subject some other way: the keyboard and screen claims test pointer identity,
and the host's process map is cleared before the job is reaped.

**The scheduler has two job tables, and only one of them is in `/proc`.** A task
userland can address — a process, a pipeline stage, init, the console pump — is
named from `1..SYS_PID_MAX`. A job the kernel runs for itself is named from
above it, and today that is the syscall server each parked call gets. They are
separated because the servers are the fire hose: one per parked syscall, ~150k a
second through a bulk pipe, which would spend the whole pid space in minutes on
tasks `Wait`, `Kill` and `Fg` cannot name — all three reach only the caller's
children, so the partition is structural rather than a range check. An anonymous
id is never held, because nothing names one past its job. The cost is that
`/proc` no longer says *which* syscall a wedged process is stuck in;
`/proc/<pid>`'s `calls` still says how many.

**0 is not a pid.** It is `sched_spawn`'s failure return, what
`tty_keys_owner()` and `tty_screen_owner()` mean by "nobody", `SYS_WAIT_ANY`,
`Fg(0)`, and `link.pid = 0` in `web/proc.js`. `/bin/sh` takes init's pid, since
it is a process inside init's task rather than a job of its own.

**`Sys::Fg` is authorised the way `kill` is, and then some**: the pid must be a
child of the caller, and the caller must have the terminal already — it holds
the raw keys, or it is itself in front, or nobody is, or **what is in front is
what it put there**. The last two clauses are not slack. A shell must let go of
the keyboard *before* it spawns, because a child runs as soon as the shell next
parks and a full-screen program claims the keys in its first step. And it arms a
pipeline a stage at a time, so from the second call onwards it holds neither the
keys nor a place in the set it is filling. The foreground therefore belongs to
whoever armed it, and the console records that rather than inferring it.

**A repaint is one operation.** `Sys::Echo` carries an anchor, a cursor offset,
a sequence of styled runs and the bytes; its reply says where the cursor ended,
what the geometry is, and how many rows the write carried the anchor up. It
authorises nothing `Sys::Style`, `Sys::Cursor` and `Sys::Write` do not, and
every byte still goes out through the process's own stdout, so a redirection
behaves. It exists because §4.4's cost falls per *operation*: a keystroke was
five round trips and is two, and Enter to the next prompt was twelve and is
five. Being one operation also keeps the intermediate states off the screen,
since the grid is presented once per tick.

**The kernel does not call a process; the host does, and never with the kernel
on the stack.** Only JS can call another instance's exports, and re-entering the
kernel from inside one of its own imports would run it on a heap it is halfway
through changing. So one `_start` or `_resume` is a *deferred host action*,
structurally identical to a storage reply: the process's proxy task parks on a
wake token, the host steps the instance once the tick has unwound, and the token
is woken with the outcome. Synchronous syscalls run the other way and re-enter
the kernel at top level, exactly as `key()` does.

**What crosses is bytes, not addresses** (Appendix B). The host asks the kernel
for room with `Sys::Stage`, copies the payload in, and only then reports the
request; the reply travels back through a block the host takes from the
process's own `_alloc`. One message each way per step, and both halves of that
protocol live in `web/proc.js` — `serveProc` is the process's side, `makeProc`
the host's — because two files describing one wire is how it drifts.

The step's reply also carries **how much memory the instance has committed**, in
`result_hi`. It belongs here rather than in an operation of its own for the
reason the ABI is this small: only the worker can read a `WebAssembly.Memory`,
the message is already being sent, and `/proc` — which publishes the figure
(§5.1) — is generated with nothing to await. It is therefore as of the last
step, which is as current as it can be: a process grows its memory only while it
runs.

**Whoever takes a worker away must fail the in-flight step**, or the kernel
parks for ever on a reply that is not coming. An abandoned request is reaped by
`wake()` on its token and by nothing else, which is why `sched_wake` returns a
bool.

A trap is how a process reports a fatal error: it has no host imports to log
through, so the kernel turns a trap into an exit status and says the process
crashed. Two fidelity losses come with the worker and neither is worth an ABI
change: a binary that will not instantiate reads as a crash (132) rather than as
"will not instantiate" (126), and `Sys::Now` is relative.

**A process ends when its root task returns**, whatever the others are doing.
The kernel then drops the instance and cancels the servers of anything still
outstanding.

### 4.4 Cost model

Compilation is expensive; instantiation is cheap. The host keeps the `Module` in
a cache keyed by path and instantiates per `exec`.
`new WebAssembly.Module(bytes)` is synchronous, which is allowed in a worker at
any size and keeps `exec` one round trip rather than two; the bytes come from
the VFS, so a binary can live in OPFS or `/home` and not only beside
`kernel.wasm`. `Module` objects are structured-cloneable, so a binary is
compiled once however many workers run it, which is why the cache stays in the
kernel worker. Starting a worker is the other cost, and the pool (§4.2) is the
answer.

**A syscall is the cost that does not go away**: two `postMessage` hops and two
copies, **measured at 34–45 µs** in three engines. A syscall-bound program pays
it per `SYS_CHUNK` (512 bytes) — a quarter of a megabyte through three processes
is 6–13 ms — which is why a bigger chunk or a batched step protocol was decided
against. What that leaves on the interactive path is the *line* rather than the
key: a keystroke is two round trips and Enter to the next prompt is five, paid
once a line, which is why it is affordable.

**The real cost is duplication.** With no dynamic linking, every binary embeds
its own copy of the allocator, the string types and the coroutine runtime, and
the shell — a language now (§4.5) — is the largest of them by some way. Keep the
process-side runtime minimal and push anything substantial into syscalls, so it
lives once in the kernel rather than N times in userland.

Cross-instance data movement is Appendix B.

### 4.5 The shell's language

`/bin/sh` is a Bourne shell, and the reference for every decision below is v7's.
It is a program under §4 like any other: nothing here is a kernel concept, and
the syscall table did not gain an operation for any of it.

**The grammar.** A line is a tree of pipelines, and `src/cmd/sh/parse.h` is the
one statement of it:

```
line     := list
list     := and_or (sep and_or)* [sep]
sep      := ';' | '&' | newline
and_or   := pipeline (('&&' | '||') pipeline)*
pipeline := ['!'] (funcdef | compound | simple ('|' simple)*)
funcdef  := name '(' ')' newline* compound
compound := group | subshell | if | loop | for | case
group    := '{' list '}'
subshell := '(' list ')'
if       := 'if' list 'then' list ('elif' list 'then' list)* ['else' list] 'fi'
loop     := ('while' | 'until') list 'do' list 'done'
for      := 'for' name ['in' word*] sep 'do' list 'done'
case     := 'case' word 'in' arm* 'esac'
arm      := ['('] word ('|' word)* ')' list [';;']
simple   := assign* (word | redirect)+ | assign+ redirect*
assign   := name '=' word, and only ahead of the first ordinary word
redirect := '<' word | '>' word | '>>' word | '2>' word | '2>>' word
          | ('<<' | '<<-') word | ('>&' | '2>&') word
```

Two bounds sit beside it. A pipeline holds at most **eight stages**, which is
what sizes the pipe and report tables the job runtime builds off it; a text
nests at most **sixteen deep**, which bounds a parser that recurses on the wasm
stack. The walk in `job.cpp` counts its own depth separately, because a tree can
be walked more deeply than it was parsed.

**A line that ends inside something is re-parsed, not lexed across a blocking
read.** The reader accumulates and asks the parser after every line whether the
text merely ended early; that answer is what draws `PS2`, and it is why there is
no lexer state machine spanning input. A here-document is the same mechanism —
its body is read by the accumulator and handed to the command as a pipe, never a
file, since one `Sys::Write` carries up to a megabyte into a single pipe slot.

**Expansion is two passes, and quote removal is in the first.** Per stage, every
word goes through parameter expansion, command substitution, splitting against
`IFS` and quote removal in one left-to-right walk, and the fields that come out
then go through filename generation. POSIX words quote removal last; here it
cannot be, because the walk is where the quoting is *known*. What crosses into
globbing instead is a per-byte mark saying which characters were quoted, which
is exactly what makes `'a*'` match a literal star while `a*` matches files.
Splitting applies to expansion output only — a literal byte of the word never
goes through it, so `IFS=:` leaves a typed `a:b` alone and cuts `$path` in two.
A redirection target and an assignment value expand as exactly one field however
they expand, so `> *.txt` writes to the pattern.

**Four things in v7 cannot exist here, and each has a decided substitute rather
than a gap.** `export` was a fifth until the environment crossed a spawn (§4.3),
and `#!` a sixth until `exec` learned to read a first line (§4.3) —
`./script.sh` works, and the row that said it never would is gone. What is left
of `export` is that an exported variable is a copy taken at spawn and there is
no `setenv` to change one after.

| v7 | Why not | What happens instead |
|---|---|---|
| `( list )` as a real subshell | There is no `fork`, and spawning a second `/bin/sh` would lose everything a spawn does not carry — the unexported variables, the functions, the options and the traps — and cost a worker against the depth cap. | It runs in this process with the shell's own mutable state saved and put back: cwd, variables, positional parameters, functions, options, traps and the `exec` base. `(cd /x; ls)` and `(set -e; …)` are exact; only memory isolation is lost. |
| A compound command in the background | Backgrounding means the shell runs on while the group does, and nothing inside a process can wait for a sibling task (§3.6). | Refused. `cmd &` on a simple pipeline is unaffected. |
| `exec cmd` replacing the image | A process is an instance in a worker; there is no re-instantiate-in-place and a spawn makes a new pid. | `exec` with no command makes its redirections permanent, which is exact. `exec cmd` spawns, waits, and leaves with the child's status. |
| `trap … <signal>` | There are no signals, and the cancellation flag is sticky: once `^C` sets it every later await answers at once. | `trap … 0` on any normal exit, and `trap … 2` in an interactive shell, where the interrupt went to the stages and this process was never cancelled. `trap '' 2` is refused rather than accepted and dropped. |

**What runs a command word is §4's rule**: a function, then a builtin, then a
binary on `PATH` — and only the third costs an instantiation and a worker.
`command -v` is the builtin that says which of the three a word is.

---

## 5. Storage

Appendix A has the full comparison of browser storage APIs and the durability
caveats.

### 5.1 The mount layering

```
OpfsFs     → OPFS                (/, and therefore everything — the store)
ProcFs     → the scheduler       (/proc, generated at open)

unbuilt:
  a File System Access Fs        (a real local directory, Chromium only, opt-in — §5.4)
  an Fs over Range requests      (read-only remote trees)
```

**Two mounts, and one of them is generated.** Everything a user can name is in
the one store: `/bin`, `/etc`, `/home`, `/tmp`, `/import` and `/pkg` are
directories in it, not filesystems of their own. There is no `/usr`, and no
`/mnt` either: a directory named for mounting would promise a second filesystem
there is no way to have. `fimport` writes the picker's bytes into `/import` like
anything else — bytes are not a filesystem.

`/pkg` is what a package manager installs into, and the archive does not carry
it — deliberately, since the unpack replaces what the archive does carry (§5.2)
and would take an installed program with it. Its layout is
[Package_Management.md](Package_Management.md)'s and the kernel knows none of
it: what reaches an installed program is `/pkg/bin` on the default `PATH` (§4),
which is a symbolic link and not a mount. Boot does not create it — a system
that installs nothing never grows one. What writes the tree is `/bin/pkg`, and
[Package_Format.md](Package_Format.md) §8 is the layout it writes.

`/bin`, `/etc` and `/README` are put there at boot by unpacking `rootfs.zip`, a
deflated zip beside `kernel.wasm` that `tools/pack.py` builds and `web/fs.js`
reads; the kernel never sees its bytes. They are therefore **writable**, which
is the price of one store: `/bin` used to be immutable because it was a
read-only archive mount, and what stands in for that now is that the archive can
always be unpacked again (§5.2).

`/proc` is `ProcFs` over the scheduler: `cwd`, `meminfo`, `mounts`, `stat`,
`tasks`, `uptime`, `version`, and one file per live pid. It is also why the
process ABI is as small as it is (§4.3) — a process reads its answers here
rather than asking for an operation — and it makes `cat` and `grep` the
introspection tools, with no second interface to keep in step. The tree is flat:
a process here has one line of state, and a generated directory level would hold
exactly one file. Content is produced at `open` and read out of that snapshot,
so a two-block read cannot describe two different moments. `/proc/cwd` is the
*kernel's* working directory; every process's own is a line in its own
`/proc/<pid>`. There is no `/proc/jobs`, because the job table is a process's
memory and no syscall shows one process another's.

`/proc/tasks` is every task with a pid at once, one line of positional fields
each, and it exists for that same snapshot rule: `ps` reformats it the way
`mount` and `df` reformat `/proc/mounts`, and a `ps` built from one read per pid
would describe as many moments as it had rows. The scheduler's anonymous jobs
(§4.3) are not here and have no file of their own, so the tree really is one
entry per live pid; `/proc/stat`'s gauges still count them, and `tasks` is
therefore larger than the number of rows. It carries what the scheduler knows
(state, what the task is suspended on, how long it has been up) beside what only
the process record does — whose child it is, how many syscalls and descriptors
it holds, how much memory it has committed against the cap it was given, and the
directory it is in. A worker is exactly a process and a process is exactly a row
with a **cwd**, so the line says whether one is bound by saying something more
useful; `/proc/<pid>` names the binding outright, for a pid with none of the
rest to show. The memory figure is the one thing here the kernel cannot see for
itself: a `WebAssembly.Memory` reports its own size and only the worker holds
one, so it comes back in every step's reply (§4.3) rather than in an operation
of its own — a file generated at `open` could not have awaited one.

`/proc/stat` is what the kernel has *done* rather than what it is holding: one
`name value` line per counter, cumulative since boot, and the reader does the
subtracting — which is why the kernel needs no notion of an interval and
`vmstat` can be an ordinary program. It is one file rather than a column added
to each of several because a rate and the gauge it is divided against have to
come from the same moment, and the snapshot rule only reaches inside one `open`.
That is also why the heap's figures appear here as well as in `/proc/meminfo`,
the way a task's appear in `/proc/tasks` as well as in `/proc/<pid>`: a
duplicated fact is cheaper than a row assembled from two moments. The first line
is `now`, the clock every counter below it was incremented against, so a reader
divides by the file's own elapsed time and not by how long it meant to wait.

A woken token is counted twice over, split by the same flag `/proc` splits a
wait with: an answer from outside is not the same event as a channel handing a
byte to its peer, and counting them together would make a pipeline look like an
interrupt storm. There is no CPU column anywhere in it, for the reason in §4.2 —
what `vmstat` prints in place of BSD's is how often the host granted the event
loop a turn.

**Every process has a working directory of its own**, inherited from whoever
spawned it and moved only by its own `chdir`. The shell's is the shell
process's; `cd` moves that, and a typed command inherits it at spawn — which is
what a redirection on that line is relative to, since the shell opens those
itself before any stage runs. A `cd` in one process moves nobody else's feet,
and that is the whole of why `cd` is a builtin. The kernel keeps one for itself,
which is where init resolves `/bin/sh` from.

What a process is *not* isolated in is the namespace: there is no per-process
root, and `open` resolves with the kernel's full authority once the path is
absolute.

### 5.2 OPFS is the primary store

The Origin Private File System is private to the origin, invisible in the user's
regular filesystem, and supported by Safari, Chrome, Edge and Firefox. It gives
real directory handles, real file handles, seekable reads and writes, truncate,
rename and remove — which maps onto §3.6's `Fs` almost one-to-one.

Rename is the one that maps least well. `FileSystemHandle.move()` is
implemented for a **file** handle alone, and not in every engine, so `Fs::rename`
answers `Err(Unsupported)` for a directory and wherever the method is missing —
and a caller that meant `mv` copies and removes instead. `web/fs.js` feature-
tests rather than naming browsers, so an engine that gains a directory `move()`
starts using it with nothing else changed. What the fast path buys is the
modification time, which §5.2 has no setter for: a copy restamps and a move
cannot.

The detail that matters most: the high-performance **synchronous**
`read()`/`write()` methods obtained via `createSyncAccessHandle()` are exposed
**only inside a Web Worker** — not the main thread, not an iframe, not even a
SharedWorker. The kernel already lives in a worker, so the fast path comes free.
**Opening** a file is async (one wake token), but once a sync access handle is
held, `read`/`write`/`getSize`/`truncate`/`flush` return immediately: those are
plain value-returning imports, the second sanctioned exception to §2.2.

**The store is unpacked from `rootfs.zip`, and stamped.** An empty store is
filled at boot without asking; after that `/etc/version` holds the
`BRAAM_VERSION` of the kernel that wrote it, and boot compares it against its
own. A mismatch is the user's decision — the prompt is on the grid before the
shell, since a stale `/bin` may be exactly what they want kept — and declining
boots on what is stored. The unpack replaces the top-level directories the
archive carries, `bin` and `etc`, and never names any other, so `/home` and
`/pkg` cannot be lost to one. The stamp is inside `etc` and written last, so an
interrupted unpack leaves none at all — and an absent stamp is an empty store,
which is unpacked without asking. A half-written image is therefore finished
rather than offered.

That is also what a writable `/bin` is held up by. `rm /bin/sh` is reachable
from the prompt and the stamp would still match, so `no_shell` offers the unpack
again rather than leaving an origin that can never boot. **The archive, not the
store, is the thing the system can be recovered from** — which is why it is
fetched lazily and never cached into the store as bytes.

**A modification time comes out of the store and cannot be put back.**
`getFile()` yields a `File`, and a `File` carries `lastModified` — milliseconds
since the epoch, at the two places the store already awaits one to learn a size,
so `Stat` and `List` carry it for nothing. What OPFS does not offer is a setter,
and it has no timestamp on a *directory* handle at all. So a directory reports 0,
which is what the whole system means by "this filesystem does not know" — `/proc`
reports it too, since a file generated at `open` has no moment to name — and
`ls -l` prints a dash there rather than 1970. `touch` on a file that already
exists is the one operation that moves a stamp, and it does it the only way there
is: the host rewrites the file with its own bytes and then reads the stamp back,
answering `Unsupported` if the browser did not restamp. Milliseconds rather than
seconds because a build step here finishes in well under one, and a `make` that
cannot tell a target from the source it was made from is no `make`.

**A symbolic link is a file the store agrees to read as one.** OPFS holds files
and directories and nothing else — a `FileSystemHandle` carries `kind` and
`name`, and there is no extended attribute, no mode and no setter for either —
so a link has to be Braam's own convention over what OPFS does offer. The two
candidates were a sidecar index per directory and a marker inside the file, and
the sidecar was rejected for the reason the per-file read-only flag was: nothing
stops a user rewriting whatever holds it, and a truth kept beside the data is a
truth that goes stale. So a link is a file whose **whole contents** are the
magic `!<braamlink>` and then the target.

The cost of that is a classification, and the bound on it is size: a file
outside `[len(magic), len(magic) + 1024]` cannot be a link, and both `stat` and
`list` already await a `File` there to learn a size. So no wasm binary in `/bin`
is ever read, and what is read is one small slice on a call that was already
asynchronous. The honest edge is that a file whose entire contents happen to be
that magic and a plausible target *is* a link as far as the system is concerned.
That is the price of a store with nowhere to put a type, and it is stated here
rather than hidden: the magic is twelve bytes chosen not to occur, and the
target must additionally be non-empty and hold no NUL.

Three things fall out of the representation rather than being built:

- **`rm` cannot follow a link**, and neither can `rm -r`. `removeEntry` sees a
  file, so it drops the link and never what it points at, and a recursive remove
  cannot walk out of the tree it was given. Nothing in the host knows about
  links in order to be right about them.
- **A link in the middle of a path announces itself.** The store walks a path a
  component at a time, and `getDirectoryHandle` on a *file* is a
  `TypeMismatchError` — `Err(NotDir)`. That is the only failure a link in the
  middle can produce, so it is also the only one worth a walk: a plain
  `Err(NotFound)` says every component above the leaf was a directory and there
  is nothing there to follow. A path with no links in it therefore costs exactly
  the one round trip it always did.
- **The open-file table is keyed on the *resolved* path.** Two names for one
  file are one entry, or the exclusive lock below would be asked for twice and
  refuse the second — which is the same rule as the one below, arrived at from
  the other direction.

Two constraints to build around:

- A sync access handle takes an **exclusive lock**, so the VFS needs an
  open-file table. The table holds **one backend handle per file and shares
  it**: a second open takes a reference on the handle that is already there
  rather than asking OPFS for one it would refuse. Offsets live above the VFS,
  so descriptors sharing a handle cannot disturb each other's position. What the
  table still refuses is a second opener while a *writer* holds the file, and a
  writer while anyone holds it — `O_TRUNC` counts as writing, since a share
  skips the backend open that would have performed it. Sharing is what makes
  that one rule on every backend: none of them is ever asked to open a file
  twice, so the rule cannot depend on which mount a path landed in.
- OPFS is unavailable in Safari private browsing. Capability-detect and
  **stop**: with the whole namespace in one store there is nothing to fall back
  to, and a memory namespace that looks like a system until the tab is reloaded
  is worse than a refusal. Boot says so on the grid and starts no shell.

### 5.3 Capability struct, not probing

The kernel asks once, at boot, and keeps the answer:

```cpp
struct StorageBackend {
    bool opfs, sync, fsaccess, persisted;
    u64  quota, usage;
};
```

`mount` consults this rather than probing at use time. It arrives as the reply
to one `Info` operation rather than being pushed in by a separate export, which
keeps the boundary to the two imports of §3.4 and lets `df` ask again for a
fresh `usage` instead of reporting a boot-time snapshot.

`persisted` is the one field the worker cannot obtain:
`navigator.storage.persist()` exists only on the main thread (§A.2). The page
calls it during boot and posts the answer down, and the worker's boot waits for
it — reporting the wrong durability is worse than a tick of delay. The wait is
*bounded*, because the call is not always a tick: the page sends a provisional
best-effort answer if the browser has not decided within a grace period, and the
real answer after it, which corrects the store. The request is made once per
page however many terminals are mounted, since persistence belongs to the
origin.

Storage semantics are inspectable from inside the OS instead of being invisible
browser behaviour, and the two halves are reported in the two places they
belong. `df` is a BSD table — blocks, used, available, capacity, per mount —
over the live `quota` and `usage`; a mount backed by the store takes the
origin's figures, since that is what backs it, and anything else answers from
`Fs::bytes()`. The backend and the mode are not per-mount facts and are not in
it: boot starts nothing without OPFS and sync handles, so neither is a thing
`df` could vary, and durability belongs to the origin. The boot banner states
both beside the quota. That line is boot's snapshot, so a `persist()` answer
arriving after it corrects the store and not the screen — the cost of keeping
the mode out of a table that is re-read all session.

### 5.4 The real local filesystem, and the escape hatch

`showDirectoryPicker()` yields a handle to an actual folder on disk, read-write,
after an explicit user gesture and permission grant. Its reach is limited
(§A.3), so it is strictly progressive enhancement, offered only where
`window.showDirectoryPicker` is defined. Directory handles are
structured-cloneable, so one can be stashed in IndexedDB and the mount
re-offered on the next visit, though permission must be re-requested each
session.

**This is unbuilt.** `mount` is an ordinary binary that reformats
`/proc/mounts`, and mounting is not something a user does: `vfs_mount` is called
from boot and nowhere else. Making it one needs the `Fs` above, a syscall or a
`/proc` write to reach it, and an answer to what a second process should see —
the namespace question §5.1 leaves open.

The universally available escape hatch is the boring one, and it is built:
`<input type="file">` for import and a Blob download for export, as `/import`
and the `fimport`/`fexport` commands. Both live on the **page** rather than in
worker, because a file picker and a download need the DOM. The picker opens
inside the transient activation of the keystroke that ran the command, which is
why `fimport` works without a button of its own.

---

## 6. Host services

Everything the browser offers that is not storage and not the terminal reaches
the kernel through the one `host_svc` import (§3.4), as an operation on the
shared request record with the object it acts on passed alongside as an
`externref`. Naming an import per operation is not the style: a new service is
an enum value on each side.

- **`fetch`** — the body comes back as a descriptor, so `read` and `close` serve
  it and nothing is duplicated.
- **WebSocket** — likewise a descriptor, written like a file.
- **The clipboard** — read and write.
- **File transfer** — the picker, opening one of its files, and an export.
- **The wall clock** — milliseconds since the epoch and the browser's offset
  from UTC. `Sys::Now` is monotonic and cannot name a day, so `date` needs this.
- **The host's description of itself** — browser, OS, architecture, cores,
  memory, locale and the raw user-agent string, as `name value` lines with a
  blank line separating what the boot banner shows from the rest. Asked once at
  boot and kept (`src/user/boot.cpp`), because `/proc/host` is generated
  synchronously and a browser does not change under a running tab. There is no
  CPU model and no clock rate in it: no browser API discloses either, and no
  user-agent string is parsed to guess — Safari still claims
  `Intel Mac OS X 10_15_7` on Apple Silicon, so a parser would report a
  confident lie. What cannot be had is left out rather than filled in.
- **Process operations** — compiling a binary, instantiating it in a worker,
  stepping it and killing it (§4.3). They are asynchronous operations on the
  host, which is this convention exactly, so they are operations here rather
  than an interface of their own; `aux` in the request record is the pid they
  name.
- **A signature check** — `crypto.subtle.verify`, Ed25519, over the index that
  names a package's hash. What it must guarantee is
  [Package_Management.md](Package_Management.md). It belongs here and not in
  wasm because WebCrypto is a promise, so it is this convention already — and
  because the host is inside the trusted base unconditionally, a verifier there
  widens nothing. A browser without the algorithm answers `Err(Unsupported)`,
  which stops `pkg` rather than skipping a check.
- **A digest is not one of these, and is compiled into `pkg` instead.**
  `crypto.subtle.digest` is one-shot: it wants the whole message, so a hash
  taken here would mean staging a whole package through `SYS_STAGE_MAX` and
  capping a package at a megabyte to keep it. SHA-256 in wasm hashes the body as
  it comes off the fetch descriptor, and nothing large crosses the boundary at
  all. The convention bends for a streaming primitive, which is the one thing an
  operation on a request record cannot express.
- **Inflate** — `DecompressionStream("deflate-raw")`, so a program can read a
  zip the way `web/fs.js` reads the archive. The compressed bytes go in and a
  descriptor comes back, so `read` and `close` serve it exactly as they serve a
  fetched body. Its input is one staged payload and is therefore capped at
  `SYS_STAGE_MAX`; its output is not capped, which is the asymmetry that makes
  the operation worth having.

Every one of them is a promise on the host side, so every one takes a wake token
and §2.2 is untouched. The wall clock is the near miss — `Date.now()` is as
synchronous as `host_now()` — but a service already had an import, and one more
operation on it costs nothing while a second value-returning import would cost
the invariant.

The clipboard, the picker and the download need the DOM, so `web/svc.js` relays
those across `postMessage` and answers by id. That is invisible from the kernel:
a service operation is a token either way.

**Reading the clipboard does not fit the pattern, and cannot.**
`navigator.clipboard.readText()` is only permitted from inside a user-gesture
handler, and a command's request reaches the page after the keystroke's handler
has returned, so the call is never in one. Safari refuses, Firefox does not
offer it to page content, and Chrome prompts. The way out is that **a paste is
itself the gesture**: the `paste` event hands the text to the page with no
permission at all, in every browser. So a refused read becomes a wait for one,
and `pbpaste` says so rather than failing. That is why `Ctrl+V` is on the
reserved list in `web/keys.js` — the kernel must not eat the keystroke that
produces the event (§3.5).

---

## 7. Repository layout

`braam_fs` and `braam_svc` are siblings above the kernel and below userland,
depending on neither the other nor upwards. `braam_ui` is in neither hierarchy:
it is linked by `braam_proc` and *not by the kernel at all*, because the
programs that paint are binaries. `src/proc` is a *different binary's* runtime —
what it shares with the kernel is four translation units and a handful of
headers.

```
doc/Concept.md          this document
doc/Release_Notes.md    reasoning behind the code, and M0–M9's acceptance criteria
doc/System_Calls.md     the kernel↔process mechanism, end to end (§4.3)
doc/Shell.md            the manual for /bin/sh: grammar, expansions, builtins, jobs (§4.5)
doc/Programming_Manual.md  the SDK's guide
doc/Package_Management.md  package signing and key-management policy (§6)
doc/Package_Format.md   the index, the anchor, a package and /pkg, byte by byte
Makefile                wrapper: all, run, serve, install, release, clean
CMakeLists.txt          the build
cmake/                  the wasm32-unknown-unknown toolchain file, BraamProgram.cmake
src/kernel/             allocator, core types, Task, scheduler, Channel, screen
src/kernel/coroutine.h  the freestanding <coroutine> shim (Appendix C)
src/kernel/hostcall.h   the asynchronous host request, shared by both interfaces
src/kernel/jsref.h      the externref table and JsRef (§3.7)
src/kernel/sysabi.h     the kernel↔process wire, included by both sides (§4.3)
src/proc/               a process binary's whole runtime: _start, syscalls, stdio
src/cmd/                one file per program, bar the two below; every program is a binary
src/cmd/pkg/            the package manager (Package_Management.md), and the main.cpp that
                        makes it a binary like any other
src/cmd/sh/             the shell (§4.5): grammar, word expander, pattern matcher, condition
                        evaluator, variables, LineEditor, job runtime, builtins, and the
                        main.cpp that makes them a binary like any other
src/fs/                 Fs interface, path, VFS, OpfsFs, storage ABI
src/svc/                fetch, WebSocket, clipboard, file transfer, clock, processes (§6)
src/ui/                 the layout layer over a Grid: Pane, TextBuf, TextView (§3.5)
src/user/               exec and the syscall dispatcher, the console and its pump, the
                        pipes behind a stage's stdio, ProcFs, boot and init
src/user/tty.h          the terminal claims: KeyInput, FullScreen
rootfs/                 the tree tools/pack.py packs into the root: /bin, /etc, /README
examples/hello/         the SDK's worked example, and an ordinary build target
test/                   in-wasm unit tests, the Node driver, and the fakes: storage,
                        services, and a process worker with no thread in it
web/                    braam.js (the embedding API), worker.js, host shim, renderer
web/proc.js             both halves of the process protocol; procworker.js is one
                        process's worker, and wiring only
tools/                  build scripts, archive packer, metadata stamper, version and
                        release scripts, size-budget check, chat server
```

---

## 8. Things to get right

### 8.1 Every awaitable is cancellation-aware
Retrofitting cancellation into coroutine code is painful. `CancelToken`
participates in every `await_suspend`, and every awaiter deregisters in its
destructor (`sched_unwait` from `~Awaiter`), which is what makes destroying a
suspended frame safe. A parking awaitable with no destructor is a
use-after-free.

### 8.2 Coroutine frame allocation is the hot path
Frames are heap-allocated per call, so the allocator is built with this as its
primary workload. A frame past 512 bytes costs a whole 64 KiB span, the
allocator's top size class, so long-lived state belongs in a heap block the
frame points at rather than in the frame.

### 8.3 Never let an import return data synchronously
Beyond the two documented exceptions (§2.2). One exception is pragmatic; three
are a second ABI.

### 8.4 `memory.grow` detaches the `ArrayBuffer`
Any cached `Uint8Array` view goes dead after a growth. Route JS-side access
through a `view()` accessor that re-derives, and make a mismatch fail loudly —
the `Screen` magic word is there for this. A host request may likewise outlive
the coroutine that issued it, so anything whose address crosses to JS is a heap
record the kernel keeps alive past a cancelled await, never a frame buffer.

### 8.5 Safari's 7-day eviction is a real hazard
With cross-site tracking prevention on, an origin that sees no user interaction
for seven days of browser use has all script-created data deleted. Mitigations:
request persistence, encourage Add to Home Screen (installed web apps are exempt
from the ITP timer), and make `fexport` easy.

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

- An origin can opt into persistent mode via `navigator.storage.persist()`,
  after which data is evicted only if the user chooses to delete it.
  **`persist()` is not available in Web Workers** — the main thread calls it
  during boot and passes the result down (§5.3).
- Quotas are generous but finite. Firefox gives best-effort origins the smaller
  of 10% of disk or a 10 GiB per-site-group limit, and persistent ones up to 50%
  of disk capped at 8 TiB; Safari's overall quota for a browser app is up to 80%
  of total disk. `navigator.storage.estimate()` is surfaced as `df`.
- **Eviction is all-or-nothing per origin.** If it fires, OPFS *and* IndexedDB
  *and* Cache go together, so there is no point using one as a backup of
  another.

### A.3 File System Access reach

Firefox and Safari ship only OPFS, and no mobile browser exposes the pickers.
Safari supports none of `showOpenFilePicker`, `showSaveFilePicker` or
`showDirectoryPicker` on macOS, iPadOS or iOS. Hence progressive enhancement
only, never a dependency.

---

## Appendix B — Cross-instance data movement

Instances cannot call each other, so every transfer is a copy through the host.
The kernel cannot be handed a buffer it did not allocate, so the host asks for
one: `Sys::Stage` is a synchronous syscall the *host* issues on the process's
behalf, returning the address of a staging block the process's kernel-side
record owns. The reverse direction needs no such call, because `_alloc` is
already in the ABI.

A process is a worker away, so the copy is in two halves with a `postMessage`
between them: the process's worker `slice`s the payload out into a transferable
`ArrayBuffer`, and the kernel's worker copies that into the staging block.
`slice` rather than `subarray` is load-bearing on both sides — a view is
detached by the next `memory.grow` (§8.4), and one that has been transferred
cannot be re-derived. The kernel half is two lines inside the per-pid
`sys_async` closure in `web/proc.js`.

**If the kernel itself is ever to do the copy, multi-memory is the tool.** A
module may declare several memories, and `memory.copy` moves bytes between two
of them. Imports are fixed at instantiation, so the kernel cannot dynamically
import a new process's memory; the trick is a tiny per-process **bridge module**
that imports both memories and exports `copy_in`/`copy_out`, about 30 bytes of
wasm instantiated alongside each process. Check `wasm-feature-detect` rather
than trusting the feature's status.

---

## Appendix C — Toolchain notes

Verified against a stock clang for `wasm32-unknown-unknown`, which has no
sysroot of its own.

### C.1 libc++'s `<coroutine>` cannot be used freestanding

It is often said that `<coroutine>` is header-only and compiler-intrinsic, so it
works freestanding as soon as `operator new` exists. That is true of the
*language feature* but not of the header: libc++'s `<coroutine>` includes
`__functional/hash.h` → `<cstring>` → `<cmath>`, which need libc declarations
(`size_t`, `memcpy`, `FP_NAN`, …) the bare `wasm32-unknown-unknown` target has
no sysroot for. A distribution that carries a wasm sysroot at all carries it per
target, and none has an `unknown-unknown` variant.

### C.2 The shim

[src/kernel/coroutine.h](../src/kernel/coroutine.h) declares
`std::coroutine_traits`, `std::coroutine_handle<>`, `std::coroutine_handle<P>`,
`std::suspend_always` and `std::suspend_never` over `__builtin_coro_resume`,
`__builtin_coro_destroy`, `__builtin_coro_done`, `__builtin_coro_promise` and
`__builtin_coro_noop`. It is 124 lines with its comments, and a deliberate part
of the foundation rather than a workaround.

`std::coroutine_traits` must be *defined*, not merely declared: a forward
declaration compiles until the first coroutine, which then fails to instantiate
it.

### C.3 The flags in §3.1

- **`--export-dynamic` is absent**, and adding it back is a regression: it is
  not a reliable way to export, having dropped a plain `extern "C"` function
  while exporting `operator new`. Exports are named individually with
  `BRAAM_EXPORT` (`export_name`), imports with `BRAAM_IMPORT`
  (`import_module`/`import_name`) — never by linker flag. Either changes the
  ABI, so the expected surface in [test/smoke/abi.mjs](../test/smoke/abi.mjs)
  changes in the same commit.
- **`--allow-undefined` is absent**, so nothing is left to resolve and an
  accidental libc dependency is a link error instead of a runtime trap.
  `memcpy`/`memset` do not leak in: bulk-memory lets LLVM lower them inline.
  Neither Homebrew nor Debian ships compiler-rt for this target, so a needed
  builtin — 128-bit division, an outlined `memcpy` — is a link error too.
- **The wasm features are named, not defaulted**, because which of them the
  default CPU turns on has changed between clang versions: `-mreference-types`
  for `__externref_t`, `-mbulk-memory` for the above, and
  `-msign-ext -mmutable-globals -mnontrapping-fptoint`. The list is verified
  sufficient by building over `-mcpu=mvp`.
- **`--no-default-config`** suppresses any `bin/clang++.cfg` a distribution
  ships, which is how a sysroot gets injected. **`--stack-first`** puts the
  shadow stack below the data segment, so an overflow traps rather than
  corrupting globals.
- **`-std=gnu++20`** rather than `c++20`, because `TRY()` is a statement
  expression.
- **`MinSizeRel`**: at `-O0` a freestanding build calls libcalls nothing
  provides.

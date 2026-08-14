# Braam

An operating system that runs in a browser tab.

Braam is a small, self-contained CLI environment — kernel, scheduler, filesystem, terminal,
shell, twenty-nine programs and six builtins — written from scratch in freestanding C++20 and
compiled to WebAssembly. It has no server side, needs no special HTTP headers, and deploys as a
static site. Nothing is linked that was not written for it: no libc, no Emscripten runtime, no
`xterm.js`. `kernel.wasm` is 165 KiB and holds no userland at all: every program is a binary of
its own, in an instance of its own.

Open the page and there is a prompt:

```
$ ls /bin                            # every program, one wasm binary each
$ echo hello > notes                 # /home survives a reload
$ curl https://example.com | less    # a real fetch, into a full-screen pager
$ edit notes                         # ^S saves, ^Q quits
$ tail -n 5 /share/doc/README | wc
$ spin &                             # a loop that yields to nobody
$ kill %1                            # dead anyway
```

It is not a Unix emulator. There is no POSIX layer, no `fork`, and no VT100 emulation, and it
does not aim to run third-party C code. Giving that up buys a system an order of magnitude
smaller, in which every mechanism is native to the browser rather than pretending to be
something else.

## The three ideas

- **C++20 coroutines are the process abstraction**, and the browser event loop is the
  scheduler. Every call that would block becomes a `co_await`, so nothing blocks and no
  stack-switching machinery — Asyncify, JSPI, threads — is needed. A suspended process is a
  coroutine frame in a hash map.
- **The terminal is a grid of cells in linear memory**, not a stream of bytes with escape
  codes in it. Colour is a struct field, cursor addressing is array indexing, and the canvas
  renderer reads the grid out of wasm memory directly.
- **A JS import never returns data — only accepts a wake token.** Results arrive later,
  through the `wake()` export. Two exceptions are sanctioned and no more, which is what keeps
  the boundary auditable: the whole of it is six imports and nine exports, asserted exactly by
  a test.

## What is in it

**A shell.** Readline-style line editing with history, quoting and escaping, and a grammar of
one pipeline per line: `|`, `<`, `>`, `>>`, `2>`, `2>>` and a trailing `&`. A pipeline's
stages run concurrently over pipes with real backpressure, `^C` reaches whatever is running
and hands the prompt back, and a nonzero exit status shows up in the next prompt. Background
jobs are managed with `jobs`, `fg` and `kill`.

**A filesystem.** A mount table over three filesystems: `MemFs` for `/` and `/tmp`, `BundleFs`
serving `/bin` and `/share` as two views of one archive loaded beside the kernel, and `OpfsFs`
on `/home` — the Origin Private File System, and the only durable one. `ProcFs` on `/proc` makes the scheduler's tasks readable as files. `df`
reports the quota, the usage, and whether the browser promised to keep the files or merely
intends to; with OPFS unavailable the system boots on memory and says so.

**Host services.** `fetch`, WebSockets, the clipboard, the file picker, downloads and a wall
clock, reached through one multiplexed import and an `externref` table the kernel owns. So
`curl` fetches, `chat` talks between two tabs over a WebSocket, `import` and `export` move
files in and out of the browser, and `pbcopy`/`pbpaste` reach the system clipboard.
`make serve` also starts [tools/wsd.mjs](tools/wsd.mjs), a dependency-free broadcast server,
so those two tabs have something real to talk through.

**A layout layer.** `Pane`, `TextBuf` and `TextView` over a grid of cells, which is what `less`
and `edit` are built out of. They run outside the kernel, so they paint a grid of their own and
send the part that changed; a full-screen program claims a keyboard route through the tty pump
rather than taking the keyboard, so `^C` always gets through.

**Isolated processes — every program is one.** There is no in-kernel program and no way to write
one: each of the twenty-nine commands in [src/cmd/](src/cmd/) is a wasm binary in an instance of
its own, and `exec` chooses between two tiers from metadata in the binary, with userland unable
to tell the difference:

| Tier | What it is | What it buys |
| --- | --- | --- |
| 2 | its own `WebAssembly.Instance` | address space, capabilities, descriptors, a 16 MB cap |
| 3 | its own instance in its own worker | a real kill switch — `worker.terminate()` |

The kernel↔process ABI is the same at either tier: two imports plus the memory the kernel caps,
four exports, and no argument anywhere for a pid — which is the whole of "a process cannot issue
a syscall on behalf of another". `spin` runs at tier 3 and exists to be un-killable by
cooperation.

Six commands are *not* programs, because no syscall could serve them: `cd` moves the one global
working directory, `jobs`, `fg` and `kill` are the shell's own job table, `exit` ends its loop,
and `help` lists the rest. They are shell builtins with no file behind them — and they are
ordinary pipeline stages all the same, so `help | grep ls` works.

**An embedding API.** `web/braam.js` puts a terminal on a host page with
`mount({ canvas })` — one instance per worker, so mounting twice gives two kernels that share
nothing but the origin's storage. `web/embed.html` does exactly that.

## Building

Requires an LLVM with the wasm32 target, plus CMake, make and Node. On macOS that is
`brew install llvm lld` — the linker is a separate formula. Nothing beyond the compiler is
taken from it: no runtime and no headers are linked or included, which is also why
[wasi-sdk](https://github.com/WebAssembly/wasi-sdk) at `/opt/wasi-sdk-33.0`, what CI uses,
works interchangeably.

```
make            # build the kernel, the binaries and the tests
make run        # run the tests
make serve      # serve the site and open it in a browser
make release    # pack the site as build/braam-<version>.zip
make clean
```

The Makefile is a wrapper; CMake is the build system, generating Unix Makefiles. Pass flags
through with `make CMAKE_ARGS=-DBRAAM_LLVM=...` — for instance if the toolchain is not where
the configure step looks for it.

`make run` is three CTest cases, and they run on every build: `smoke` asserts the exact
import and export surface of `kernel.wasm` and of each binary and then boots the kernel under
Node, `unit` runs the in-wasm unit tests, and `size` enforces the per-binary budgets in
[tools/size_budget.txt](tools/size_budget.txt). The host side is faked in
[test/](test/) — including the tier-3 worker protocol, which CI runs end to end over a link
with no thread in it.

The build leaves a self-contained static site in `build/web/`. It needs no server and no
special headers, so copying that directory anywhere is a deployment. `make release` packs
exactly that directory as `build/braam-<version>.zip`, which unpacks to one
`braam-<version>/` directory — put it in a web root and the site is at
`https://example.org/braam-<version>/`. Nothing in it configures the server: the loader falls
back to a buffered instantiate where the host does not serve `.wasm` as `application/wasm`.

## Layout

| Directory | What is in it |
| --- | --- |
| [src/kernel/](src/kernel/) | coroutines, allocator, scheduler, screen, channels, the JS boundary |
| [src/fs/](src/fs/) | paths, the VFS, `MemFs`/`BundleFs`/`OpfsFs`, the storage ABI |
| [src/svc/](src/svc/) | fetch, WebSocket, clipboard, file transfer, clock, process control |
| [src/ui/](src/ui/) | the layout layer over a `Grid`: `Pane`, `TextBuf`, `TextView` |
| [src/user/](src/user/) | line editor, grammar, job runtime, shell, `exec`, `ProcFs`, boot, builtins |
| [src/proc/](src/proc/) | a process binary's runtime |
| [src/cmd/](src/cmd/) | one file per program; every program is a binary |
| [web/](web/) | the page, the worker, the renderer, the host side of every ABI |

## Documentation

[doc/Concept.md](doc/Concept.md) is the specification — the architecture and the reasoning
behind each decision. Read it first. [doc/Milestones.md](doc/Milestones.md) is the plan that
was followed: M0–M9, with acceptance criteria and a note on how each milestone departed from
its plan. [doc/Release_Notes.md](doc/Release_Notes.md) explains, per milestone, why the code
that exists looks the way it does — comments in the source say *what*, and that file says
*why*. [doc/System_Calls.md](doc/System_Calls.md) is the one walkthrough: how a user process
talks to the kernel, from the principles down to the wire, with sequence diagrams of the calls
that actually happen and the whole syscall table in one place.

## Status

Complete, as a first version. All ten milestones are done, M0 through M9: the nucleus, the
scheduler, the screen and keyboard, the shell, streams, the filesystem, host services, the
layout layer and job control, isolated processes, and liveness isolation. Every acceptance
criterion is ticked and the test suite passes. Since then the applet tier has been retired and
every program is a binary of its own: `kernel.wasm` is about 169 KB against a 256 KiB budget,
and the boot archive that carries the twenty-nine binaries is 370 KB.

What is deliberately absent is recorded rather than forgotten: no `bg` and no `^Z` (stopping
a running coroutine is the resume-side twin of cancellation and would have to reach every
awaitable), no re-wrapping of logical lines on resize, no per-process working directory, no
window manager over the pane primitive, and no CPU metering — a tier-3 program can be killed
but not bounded.

## License

MIT. See [LICENSE](LICENSE).

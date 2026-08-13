# Braam

An operating system that runs in a browser tab.

Braam is a small, self-contained CLI environment — kernel, scheduler, filesystem, terminal and
shell — written from scratch in freestanding C++20 and compiled to WebAssembly. It has no
server side, needs no special HTTP headers, and deploys as a static site.

It is not a Unix emulator. There is no POSIX layer, no libc, and no VT100 emulation, and it
does not aim to run third-party C code. Giving that up buys a system an order of magnitude
smaller, in which every mechanism is native to the browser rather than pretending to be
something else:

- **C++20 coroutines are the process abstraction**, and the browser event loop is the
  scheduler. Every call that would block becomes a `co_await`, so nothing blocks and no
  stack-switching machinery — Asyncify, JSPI, threads — is needed.
- **The terminal is a grid of cells in linear memory**, not a stream of bytes with escape
  codes in it. Colour is a struct field; cursor addressing is array indexing.
- **Processes can be isolated by trust.** Because a WebAssembly instance cannot reach outside
  its own linear memory or call an import it was not given, one instance per process yields
  address-space and capability isolation with a hard memory ceiling.

## Status

Early. Milestone M0 — the nucleus — is done: a freestanding wasm build, a hand-written
`<coroutine>` shim, an allocator built for coroutine frames, the base core types, and a page
that boots the kernel in a Web Worker and prints a line. M1 — the scheduler — is done too:
`Task<T>`, a ready queue, kernel-side timers, wake tokens, and cancellation that unwinds a
sleeping task by returning through it. M2 — the screen and keyboard — is done as well: a grid of
cells in linear memory that a canvas renderer reads directly, damage rectangles, `Channel<T>`,
and a keyboard that speaks Unicode codepoints rather than control characters. M3 — the userland
shell — is done: a `LineEditor` coroutine with history and readline-style editing, a tokeniser, a
program registry that each program adds itself to, argv and exit codes. M4 — streams — is done:
pipes with real backpressure, stdin, the whole shell grammar including quoting and redirection,
and a `^C` that reaches a running pipeline. Open the page and there is a prompt: `ls | grep foo`
runs both programs at once over a bounded pipe, `ls | head -n 2` stops the producer early,
`^C` interrupts whatever is running and hands the prompt back, and a failed command shows its
status in the next one. Thirteen programs. `kernel.wasm` is 61 KB.
M5, the filesystem, is next.

[doc/Concept.md](doc/Concept.md) is the specification — the architecture and the reasoning
behind each decision. Read it first. [doc/Milestones.md](doc/Milestones.md) is the plan: the
milestone sequence M0–M9 with acceptance criteria. [doc/Release_Notes.md](doc/Release_Notes.md)
explains why the code that exists looks the way it does.

## Building

Requires an LLVM with the wasm32 target, plus CMake, make and Node. On macOS that is
`brew install llvm lld` — the linker is a separate formula. Nothing beyond the compiler is
taken from it: no runtime and no headers are linked or included, which is also why
[wasi-sdk](https://github.com/WebAssembly/wasi-sdk) at `/opt/wasi-sdk-33.0`, what CI uses,
works interchangeably.

```
make            # build the kernel and the tests
make run        # run the tests
make serve      # serve the site and open it in a browser
make clean
```

The Makefile is a wrapper; CMake is the build system, generating Unix Makefiles. Pass flags
through with `make CMAKE_ARGS=-DBRAAM_LLVM=...` — for instance if the toolchain is not at
`/opt/wasi-sdk-33.0`.

The build leaves a self-contained static site in `build/web/`. It needs no server and no special
headers, so copying that directory anywhere is a deployment.

## License

MIT. See [LICENSE](LICENSE).

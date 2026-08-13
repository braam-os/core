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
and a keyboard that speaks Unicode codepoints rather than control characters. Type into the page
and the characters appear; resize the window and the screen reflows. `kernel.wasm` is 14 KB.
M3, the userland shell, is next.

[doc/Concept.md](doc/Concept.md) is the specification — the architecture and the reasoning
behind each decision. Read it first. [doc/Milestones.md](doc/Milestones.md) is the plan: the
milestone sequence M0–M9 with acceptance criteria. [doc/Release_Notes.md](doc/Release_Notes.md)
explains why the code that exists looks the way it does.

## Building

Requires [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) — used as a clang distribution
only, since none of its runtime or headers are linked — plus CMake, make and Node.

```
make            # build the kernel and the tests
make run        # run the tests
make serve      # serve the site and open it in a browser
make clean
```

The Makefile is a wrapper; CMake is the build system, generating Unix Makefiles. Pass flags
through with `make CMAKE_ARGS=-DBRAAM_WASI_SDK=...` — for instance if the SDK is not at
`/opt/wasi-sdk-33.0`.

The build leaves a self-contained static site in `build/web/`. It needs no server and no special
headers, so copying that directory anywhere is a deployment.

## License

MIT. See [LICENSE](LICENSE).

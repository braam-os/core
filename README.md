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
that boots the kernel in a Web Worker and prints a line. `kernel.wasm` is 4 KB. M1, the
scheduler, is next.

[doc/Concept.md](doc/Concept.md) is the specification and the development plan, including the
milestone sequence M0–M9 and the reasoning behind each decision. Read it first.
[doc/Release_Notes.md](doc/Release_Notes.md) explains why the code that exists looks the way it
does.

## Building

Requires [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) — used as a clang distribution
only, since none of its runtime or headers are linked — plus CMake, Ninja and Node.

```
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-unknown-unknown.cmake
cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target serve      # then open http://localhost:8080/
```

Pass `-DBRAAM_WASI_SDK=<path>` if the SDK is not at `/opt/wasi-sdk-33.0`. The build leaves a
self-contained static site in `build/web/`; it needs no server and no special headers, so
copying that directory anywhere is a deployment.

## License

MIT. See [LICENSE](LICENSE).

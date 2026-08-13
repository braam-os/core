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

Early. The design is settled and written up; implementation has not started.

[doc/Concept.md](doc/Concept.md) is the specification and the development plan, including the
milestone sequence M0–M9 and the reasoning behind each decision. Read it first.

## Building

Requires [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) — used as a clang distribution
only, since none of its runtime or headers are linked. There is no build system yet; it
arrives with milestone M0. See Appendix C of the concept document for the verified compiler
invocation.

## License

MIT. See [LICENSE](LICENSE).

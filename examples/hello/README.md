# hello — a Braam program

Build it against an installed SDK, with the toolchain file the SDK ships:

    cmake -B build --toolchain <prefix>/lib/cmake/braam/wasm32-unknown-unknown.cmake
    cmake --build build

The toolchain is what makes the compiler a wasm32 one, and CMake picks the
compiler when the project is first configured — so it must be on that first
command. A build directory configured without it cannot be repaired by adding
it; delete the directory and configure again.

`build/hello.wasm` is the program: a wasm module importing `kernel.sys`,
`kernel.sys_async` and `env.memory`, exporting `_start`, `_resume`, `_alloc` and
`_free`, and carrying a `braam` custom section that says which process ABI it
was built for.

Run it on a Braam that is already up — no rebuild of the system:

- **Through the file picker.** Type `fimport`, choose `hello.wasm`. It lands in
  `/import/`. Then `/import/hello.wasm`.
- **Over the network.** `curl https://example.com/hello.wasm > /home/hello`,
  then `/home/hello`.

Either way it prints `Hello, world!`, or `Hello, Serge!` if given a name.

The full guide is `share/doc/braam/Programming_Manual.md`.

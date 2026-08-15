# Writing a program for Braam

Every command in Braam is a wasm binary of its own, and nothing about that is private to
this repository. A program is one C++ file, one `#include`, and one function; it is compiled
by a plain clang against a handful of headers, linked against two static libraries, and
stamped with a note saying which process ABI it was built for. This document is the whole of
what somebody outside the tree needs.

It is the out-of-tree half of [System_Calls.md](System_Calls.md) §12, which describes the
same thing from inside `src/cmd/`. The mechanism underneath — what a syscall is, how the
kernel answers it, what a descriptor is — is that document; this one is the build and the
API.

---

## 1. Installing the SDK

From a source tree:

```
make install                 # /usr/local if it is writable, else ~/.local
make install PREFIX=/opt/braam
```

Or unpack `braam-sdk-<version>.zip` from a release anywhere at all. The tree is relocatable:
the CMake package finds its own prefix by walking up from itself, so an unpacked archive is
a working SDK with nothing installed and no environment variable set.

Either way, this is what you get:

| Path | What it is |
| --- | --- |
| `include/braam/{kernel,fs,proc,ui}/` | the headers a program includes |
| `lib/braam/libbraam_proc.a` | the process runtime: the allocator, the strings, the task scheduler, the syscall wrappers |
| `lib/braam/libbraam_ui.a` | the layout layer, for a program that paints |
| `lib/cmake/braam/wasm32-unknown-unknown.cmake` | the toolchain file |
| `lib/cmake/braam/braamConfig.cmake` | `find_package(braam)` |
| `lib/cmake/braam/BraamProgram.cmake` | `braam_add_program()` |
| `libexec/braam/stamp.py` | the post-link stamp |
| `share/braam/examples/hello/` | the example below |
| `share/doc/braam/Programming_Manual.md` | this file |

You also need what Braam itself needs: a clang with the wasm32 target and `wasm-ld` beside
it (`brew install llvm lld`, or `apt install clang lld llvm`), CMake 3.24, and Python 3 for
the stamp. Nothing is taken from the clang distribution but the compiler — no runtime, no
headers, no sysroot. There is no libc here and there is no way to add one.

---

## 2. Hello, world

```cpp
// hello.cpp
#include "proc/io.h"

Task<i32> proc_main(Args args)
{
    Str who = "world";
    if (args.size() > 1)
        who = args[1];

    if ((co_await write_all(SYS_STDOUT, "Hello, ")).is_err() ||
        (co_await write_all(SYS_STDOUT, who)).is_err() ||
        (co_await write_all(SYS_STDOUT, "!\n")).is_err())
        co_return 1;

    co_return 0;
}
```

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.24)
project(hello LANGUAGES CXX)

find_package(braam REQUIRED)

braam_add_program(NAME hello SOURCES hello.cpp)
```

```
cmake -B build --toolchain <prefix>/lib/cmake/braam/wasm32-unknown-unknown.cmake
cmake --build build
```

`build/hello.wasm` is the program, about 6 KB.

**The toolchain file is the one thing you have to name, and only on that first command.** It
is what makes the compiler a wasm32 one, and it points `find_package` at the SDK it was taken
from, so nothing else has to be spelled out — not the include path, not the libraries, not
`stamp.py`. `--toolchain` is CMake's shorthand for `-DCMAKE_TOOLCHAIN_FILE=`.

CMake chooses the compiler when a project is first configured, so a build directory
configured *without* it holds a host compiler and cannot be repaired by adding the flag: you
get `sizeof(usize) == 4, "wasm32"` and `unknown type name '__externref_t'` from the headers,
or, since `find_package(braam)` refuses a non-wasm32 compiler, a message saying so. Delete
the build directory and configure again.

`braam_add_program(NAME <n> SOURCES <...> [TIER 3|2] [LIBS <...>])` is the same function
`src/cmd/` builds the system's own thirty-two programs with. It links `braam::proc` and
`braam::flags`, links with `--import-memory` so the memory cap is the kernel's, and runs
`stamp.py` over the result. `LIBS` names anything else the program is made of; `TIER` is §7
below, and you will not need it. The CMake target it defines is `bin_<name>` — the file is
`<name>.wasm`, and the prefix is there because a program may be called `test` or `install`.

---

## 3. Running it

A program does not have to be in the system image to run. `exec` resolves a path through the
ordinary filesystem against the calling process's working directory, and accepts anything
carrying a well-formed stamp — so a `.wasm` that arrives at runtime is a command.

**Through the file picker.** At the prompt, `import`, and choose `hello.wasm`. It lands in
`/mnt/import/`:

```
$ import
/mnt/import/hello.wasm
$ /mnt/import/hello.wasm Serge
Hello, Serge!
```

**Over the network**, into the one durable filesystem:

```
$ curl https://example.org/hello.wasm > /home/hello
$ /home/hello
Hello, world!
```

A bare word with no slash in it means `/bin/<word>`, and `/bin` is a read-only view of the
archive loaded beside the kernel — so putting a program *there* does mean rebuilding the
image. Anywhere else needs nothing.

One thing to know while iterating: the host caches a compiled module by path, so replacing a
program at a path that has already been run in this page does not take effect until a reload.
Write the new one beside the old, or reload.

---

## 4. The shape of a program

`proc_main` is what a program defines, and its return value is the exit status. Everything
that would block is a `co_await`, because a process is a coroutine and nothing anywhere
blocks — that is Concept.md §2.1 and it reaches all the way down here.

A filter is the shape most programs have:

```cpp
Task<i32> proc_main(Args args)
{
    Input in(args.tail(), SYS_STDIN);      // the named files, or stdin
    if (i32 bad = co_await in.open_all("count"))
        co_return bad;                     // a missing file, before any output

    usize lines = 0;
    LineReader lr(in);
    String line;
    for (;;) {
        Result<bool> r = co_await lr.next(line);
        if (r.is_err())
            co_return r.error() == Error::Cancelled ? 130 : 1;
        if (!r.value())
            break;                         // end of input
        lines++;
    }

    Buf<24> b;
    b.put(lines).put('\n');
    if ((co_await write_all(SYS_STDOUT, b.str())).is_err())
        co_return 1;
    co_return 0;
}
```

Four conventions there, and every program in `src/cmd/` follows them:

- `Input` decides files-or-stdin in its constructor, and `open_all` returns the exit status
  directly, so a missing file is reported before anything is printed.
- **`Error::Closed` is a normal end of input**, not a failure.
- **`Error::Cancelled` is `^C`**, and the exit status for it is 130.
- Output is formatted into a stack `Buf<N>` and written once. A write per field is a syscall
  per field.

There is no `main`, no `argc`/`argv`, no `printf`, no `errno` and no exceptions. `args[0]` is
the name the program was invoked by; `args.tail()` is everything after it.

---

## 5. The API

### `proc/io.h` — one wrapper per syscall

Each is a `Task<Result<T>>`. `Result` carries an `Error` and is unpacked with `.is_err()`,
`.error()` and `.value()`; `CO_TRY` propagates one.

| Group | What is there |
| --- | --- |
| Streams | `write_all(fd, Str)`, `read_chunk(fd)`, `close_fd(fd)` |
| Files | `open_at(path, flags)`, `open_read`, `read_file`, `stat_of`, `list_dir`, `make_dir`, `remove_path` |
| Directory | `cwd_get()`, `cwd_set(path)` — this process's own, inherited from whoever spawned it |
| Children | `make_pipe()`, `spawn(Args, ChildIo)`, `wait_child(pid)`, `kill_child(pid)`, `set_fg(pid)` |
| Terminal | `keys_claim(bool)`, `screen_claim(bool)`, `key_read()`, `cursor_get()`, `cursor_set(x, y, on)`, `style_set(fg, bg, attrs)` |
| System | `storage_of()`, `sleep_for(ms)`, `clock_now()`, `proc_pid()`, `proc_now()` |
| Host services | `fetch_url(url, spec)`, `ws_connect(url)`, `clip_get`, `clip_put`, `pick`, `pick_open`, `save` |
| Helpers | `errln(who, what, why)`, `Input`, `LineReader`, `next_line`, `next_field` |

Everything that is a stream of bytes comes back as a descriptor, so there is nothing new to
learn for any of it: a fetched body is read with `read_chunk` until `Err(Closed)` and closed
with `close_fd`, and a WebSocket is written with `write_all`.

Two rules about descriptors, both of which the kernel enforces rather than trusts:

- **A descriptor named in a spawn is *moved* into the child.** That is what closes this side
  of a pipe, and therefore what lets the other side see an end of input. It must not be used
  after the spawn.
- **One user of a descriptor in one direction at a time.** A second concurrent read of the
  same descriptor is `Err(Perm)`.

A process may have up to four syscalls outstanding at once, across as many tasks as it has;
`proc_spawn(Task<i32>)` starts a second task, and the process ends when the *root* task
returns, whatever the others are doing.

### `proc/screen.h` and `ui/` — painting

A full-screen program claims the alternate screen and the keyboard, paints into a `Grid` of
its own, and sends the damage across in one syscall per frame:

```cpp
ProcScreen s;
co_await s.take_screen();
co_await s.take_keys();
Pane body = s.body();
body.write_at(0, 0, "hello");
co_await s.flush();
Key k = co_await s.next_key();
```

`Pane` clips, never wraps and never scrolls; `TextBuf` holds lines and `TextView` scrolls a
window over one. That is what `less` and `edit` are built out of, and it links into the
binary rather than living in the kernel. A resize rides on every key reply, so there is no
event to subscribe to.

Nothing gives a claim back on your behalf — but nothing has to: a process that dies has its
claims released by the kernel, because a killed program runs no destructor.

### What the headers do *not* contain

`include/braam/kernel/` and `include/braam/fs/` are shipped because the two libraries'
headers include them, and they are worth reading — `str.h`, `string.h`, `vec.h`, `span.h`,
`result.h`, `fmt.h`, `text.h`, `path.h` are the whole standard library here. But the parts of
them that name the scheduler, the host imports or the VFS belong to the kernel and have
nothing behind them in a program: reaching one is a link error, which is the intended answer.

---

## 6. The rules that bite

These come from Concept.md §2 and §C.3, and each of them is a compile error, a link error or
a trap rather than a warning:

- **No exceptions and no RTTI.** Errors are `Result<T, E>`.
- **No libc.** No `malloc`, no `memcpy` you did not write, no `<cstring>`. `-nostdlib
  -nostdinc++` is not negotiable, and a construct needing a compiler-rt builtin — 128-bit
  division, an outlined `memcpy` — will not link.
- **Never `new` anything.** `operator new` returns null on failure and there are no
  exceptions, so the expression would construct at address zero. Use `heap_new` and
  `heap_delete` from `kernel/alloc.h`.
- **A namespace-scope global must be trivially destructible.** A non-trivial destructor pulls
  in `__cxa_atexit`, which nothing provides. Make the state a POD, or put it behind a pointer
  built on first use.
- **Keep coroutine frames small.** A frame past 512 bytes costs a whole 64 KiB span from the
  allocator's top size class. Long-lived state belongs in a heap block the frame points at,
  not in the frame.
- **The memory cap is 16 MB**, and it is the kernel's number, not the binary's:
  `--import-memory` with no declared maximum means the host supplies the `Memory` and its
  ceiling.

---

## 7. The worker

**Your program runs in a Web Worker of its own**, and `braam_add_program` arranges that with
nothing asked of you. It is what every program in `/bin` gets, `/bin/sh` included. What it buys
is `worker.terminate()`: a kill that does not need the program's cooperation, so a bug that loops
for ever costs a command rather than the session.

The cost is that every syscall becomes two `postMessage` hops rather than a call — 34–45 µs
measured, paid per `SYS_CHUNK` — so a program that reads a large file pays it per 512 bytes, and
one being typed into pays it per round trip its editor makes. A program in that position may give
the worker up and run in the kernel's instead:

```cmake
braam_add_program(NAME repl SOURCES repl.cpp TIER 2)
```

It is still a process — its own instance, its own address space, capabilities and descriptors,
and the same memory cap. What it gives up is only the kill: such a program that stops answering
hangs the kernel's worker, so ask for it only where a runaway is not a possibility a user has to
live with. Nothing in the system asks for it today; the shell was the last, and cutting the round
trips it made was the cheaper answer than weakening its isolation.

You do not have to handle the case where the host has no workers to give. The same binary runs in
the kernel's worker there, with no change to the program and nothing to detect.

---

## 8. Versioning

The stamp carries a process-ABI number, and the kernel checks it before it runs anything.
`stamp.py` reads that number out of the `kernel/sysabi.h` the SDK shipped, so a binary is
stamped with the ABI of the headers it was actually built against — never a restated
constant that could fall behind.

That is what makes a mismatch a sentence rather than a crash:

```
$ ./hello
sh: hello: built for another process ABI
```

The answer is to rebuild against the SDK that matches the system. `not executable` is the
other one, and it means the file has no stamp at all — an ordinary `.wasm` from somewhere
else, or a stamp that was stripped.

The ABI changes when the syscall table does, and both are documented in Concept.md §4.3.

---

## 9. Checking a binary by hand

The build produces a module with an exact surface, and it is worth knowing what it is,
because a link that accidentally pulled in kernel code shows up here first:

- **Imports** are `env.memory`, `kernel.sys` and `kernel.sys_async` — and nothing else. Any
  other import means the process ABI has been gone around. `sys_async` is absent from a
  program that never awaits.
- **Exports** are exactly `_alloc`, `_free`, `_resume`, `_start`. `memory` is *imported*, not
  exported, which is what makes the cap the kernel's.
- **One custom section named `braam`**, six little-endian `u32`s: magic `0x6d617262`, the
  ABI, the tier, flags, the initial pages and the maximum.

```
$ node -e 'const m=new WebAssembly.Module(require("fs").readFileSync("build/hello.wasm"));
  console.log(WebAssembly.Module.imports(m).map(i=>i.module+"."+i.name));
  console.log(WebAssembly.Module.exports(m).map(e=>e.name));
  console.log(new Uint32Array(WebAssembly.Module.customSections(m,"braam")[0]))'
```

In the Braam source tree, `test/run.mjs` asserts all of that for every binary, and will do it
for a binary of yours if you hand it one:

```
node test/run.mjs --kernel build/kernel.wasm build/web/bundle.bin /path/to/hello.wasm
```

---

## 10. Where to read next

- [System_Calls.md](System_Calls.md) — the mechanism end to end: the deferred step, the
  staging protocol, cancellation, the kill, and the whole syscall table in one place.
- [Concept.md](Concept.md) — the specification. §4.3 is the process ABI, §4.4 is what a
  process costs, §2 is the three invariants everything else follows from.
- `src/cmd/` in the source tree — thirty-two worked examples, from `true.cpp` at six lines
  to the shell.

# Writing a program for Braam

Every command in Braam is a wasm binary of its own, and nothing about that is
private to this repository. A program is one C++ file, one `#include`, and one
function; it is compiled by a plain clang against a handful of headers, linked
against two static libraries, and stamped with a note saying which process ABI
it was built for. This document is the whole of what somebody outside the tree
needs.

It is the out-of-tree half of [System_Calls.md](System_Calls.md) §12, which
describes the same thing from inside `src/cmd/`. The mechanism underneath — what
a syscall is, how the kernel answers it, what a descriptor is — is that
document; this one is the build and the API.

---

## 1. Installing the SDK

From a source tree:

```
make install                 # /usr/local if it is writable, else ~/.local
make install PREFIX=/opt/braam
```

Or unpack `braam-sdk-<version>.zip` from a release anywhere at all. The tree is
relocatable: the CMake package finds its own prefix by walking up from itself,
so an unpacked archive is a working SDK with nothing installed and no
environment variable set.

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

You also need what Braam itself needs: a clang with the wasm32 target and
`wasm-ld` beside it (`brew install llvm lld`, or `apt install clang lld llvm`),
CMake 3.24, and Python 3 for the stamp. Nothing is taken from the clang
distribution but the compiler — no runtime, no headers, no sysroot. There is no
libc here and there is no way to add one.

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

**The toolchain file is the one thing you have to name, and only on that first
command.** It is what makes the compiler a wasm32 one, and it points
`find_package` at the SDK it was taken from, so nothing else has to be spelled
out — not the include path, not the libraries, not `stamp.py`. `--toolchain` is
CMake's shorthand for `-DCMAKE_TOOLCHAIN_FILE=`.

CMake chooses the compiler when a project is first configured, so a build
directory configured *without* it holds a host compiler and cannot be repaired
by adding the flag: you get `sizeof(usize) == 4, "wasm32"` and
`unknown type name '__externref_t'` from the headers, or, since
`find_package(braam)` refuses a non-wasm32 compiler, a message saying so. Delete
the build directory and configure again.

`braam_add_program(NAME <n> SOURCES <...> [LIBS <...>])` is the same function
`src/cmd/` builds the system's own thirty-six programs with. It links
`braam::proc` and `braam::flags`, links with `--import-memory` so the memory cap
is the kernel's, and runs `stamp.py` over the result. `LIBS` names anything else
the program is made of. The CMake target it defines is `bin_<name>` — the file
is `<name>.wasm`, and the prefix is there because a program may be called `test`
or `install`.

---

## 3. Running it

A program does not have to be in the system image to run. `exec` resolves a path
through the ordinary filesystem against the calling process's working directory,
and accepts anything carrying a well-formed stamp — so a `.wasm` that arrives at
runtime is a command.

**Through the file picker.** At the prompt, `fimport`, and choose `hello.wasm`.
It lands in `/import/`:

```
$ fimport
/import/hello.wasm
$ /import/hello.wasm Serge
Hello, Serge!
```

**Over the network**, into the one durable filesystem:

```
$ curl https://example.org/hello.wasm > /home/hello
$ /home/hello
Hello, world!
```

A bare word with no slash in it means `/bin/<word>`, and `/bin` is a read-only
view of the archive loaded beside the kernel — so putting a program *there* does
mean rebuilding the image. Anywhere else needs nothing.

One thing to know while iterating: the host caches a compiled module by path, so
replacing a program at a path that has already been run in this page does not
take effect until a reload. Write the new one beside the old, or reload.

---

## 4. Writing a script

**Not every command has to be a binary.** `/bin/sh` is a Bourne shell —
variables, `if`, the three loops, `case`, functions, globbing, `$( )`,
here-documents, `test` and `trap` — so a command whose work is running other
commands is a text file, and needs no toolchain at all. The grammar is
Concept.md §4.5, and [Shell.md](Shell.md) is the whole shell in detail.

Write it anywhere the filesystem reaches — `edit` is in `/bin` — and run it by
name after `sh`:

```
$ cat /home/report.sh
show() {
  echo "$1: $(wc < $1)"
}
trap 'echo done' 0
for f in /home/*.txt
do
  case $f in
  *draft*) continue ;;
  esac
  show $f
done
$ sh /home/report.sh
/home/notes.txt: 12 84 501
/home/todo.txt: 3 9 46
done
```

Arguments after the file are `$1` onwards and the file itself is `$0`, exactly
as `sh -c 'cmd' name args` takes its own after the command string. `sh -s` reads
a script off standard input instead, which is what a pipeline into the shell
uses. The status the script leaves with is the status `sh` exits with, so a
script composes into a pipeline like anything else.

**`#!` works, within three bounds.** A file whose first line is `#!` followed by
an absolute path is executable, so `./script.sh` and a script installed in
`/bin` both run:

```
$ cat greet
#!/bin/sh
echo greetings $1
$ ./greet world
greetings world
```

What is instantiated is the interpreter, entered with itself, its one argument
if the line carried one, the resolved path of the script, and then your own
arguments — so `$0` is the script and `$1` onwards are yours, exactly as
`sh file` gives them. The interpreter must be **absolute**, since there is no
PATH to search; the lookup is **one level deep**, so an interpreter that is
itself a script is refused; and the first line must end within 128 bytes.
`test -x` answers by the same two rules, and a script costs one process rather
than two.

**One limit remains, and it is deliberate.** A script is **parsed whole before
any of it runs**, as `. file` is, so a syntax error on the last line means the
first line does not run either. That is the price of the shell keeping its
standard input free for the script to read from, which is what lets
`while read l; do …; done` inside one work against a pipe.

The rest of what a script can and cannot do — no subshell isolation, no compound
command in the background — is §4.5's table. `export` does reach a child: an
exported variable is copied into every command's environment at spawn, and a
nested `sh` reads it back into its own table.

---

## 5. The shape of a program

`proc_main` is what a program defines, and its return value is the exit status.
Everything that would block is a `co_await`, because a process is a coroutine
and nothing anywhere blocks — that is Concept.md §2.1 and it reaches all the way
down here.

A filter is the shape most programs have:

```cpp
Task<i32> proc_main(Args args)
{
    Input in(args.tail(), SYS_STDIN, "count"); // the named files, or stdin

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

- `Input` decides files-or-stdin in its constructor, and opens each named file
  only when the read reaches it, closing it before the next. A file that will
  not open prints `count: <path>: <why>` on stderr itself and comes back as an
  ordinary error, so it is reported where the reading stopped rather than before
  any output.
- **`Error::Closed` is a normal end of input**, not a failure.
- **`Error::Cancelled` is `^C`**, and the exit status for it is 130.
- Output is formatted into a stack `Buf<N>` and written once. A write per field
  is a syscall per field.

There is no `main`, no `argc`/`argv`, no `printf`, no `errno` and no exceptions.
`args[0]` is the name the program was invoked by; `args.tail()` is everything
after it.

---

## 6. The API

### `proc/io.h` — one wrapper per syscall

Each is a `Task<Result<T>>`. `Result` carries an `Error` and is unpacked with
`.is_err()`, `.error()` and `.value()`; `CO_TRY` propagates one.

| Group | What is there |
| --- | --- |
| Streams | `write_all(fd, Str)`, `read_chunk(fd)`, `close_fd(fd)` |
| Files | `open_at(path, flags)`, `open_read`, `read_file`, `stat_of`, `list_dir`, `make_dir`, `remove_path`, `touch_path`, `make_link`, `read_link` |
| Directory | `cwd_get()`, `cwd_set(path)` — this process's own, inherited from whoever spawned it |
| Children | `make_pipe()`, `spawn(Args, ChildIo, const Args *env)`, `wait_child(pid)`, `kill_child(pid)`, `set_fg(pid)` |
| Terminal | `tty_of(fd)`, `keys_claim(bool)`, `screen_claim(bool)`, `key_read()`, `cursor_get()`, `cursor_set(x, y, on)`, `style_set(fg, bg, attrs)`, `cursor_echo(x, y, cur, flags, runs)` |
| System | `storage_of()`, `sleep_for(ms)`, `clock_now()`, `proc_pid()`, `proc_now()` |
| Environment | `proc_env(name)`, `proc_env_count()`, `proc_env_at(i)` — from `proc/rt.h`, and not syscalls |
| Host services | `fetch_url(url, spec)`, `ws_connect(url)`, `clip_get`, `clip_put`, `pick`, `pick_open`, `fexport` |
| Helpers | `errln(who, what, why)`, `Input`, `LineReader`, `next_line`, `next_field` |

Everything that is a stream of bytes comes back as a descriptor, so there is
nothing new to learn for any of it: a fetched body is read with `read_chunk`
until `Err(Closed)` and closed with `close_fd`, and a WebSocket is written with
`write_all`.

The environment came in with `_start`, so reading it costs nothing:
`proc_env("HOME")` reports the value or an empty `Str`, and `proc_env_at` walks
the lot as `NAME=value` words. It is a copy taken at the spawn and **cannot be
changed** — there is no `setenv`, and nothing a program does reaches its parent
or a child already running. What a program *can* choose is what its own children
get: `spawn(argv, io)` hands them this process's, and `spawn(argv, io, &env)`
hands them exactly the words named. `/bin/env` is the worked example of both.

`stat_of` answers a `FileInfo{kind, size, mtime}` and `list_dir` a `DirEntry`
each, which is the same three fields plus a name. The `mtime` is milliseconds
since the epoch, and **0 means the filesystem keeps none**: every directory,
since OPFS has no timestamp on one, and all of `/proc`, which is generated at
`open`. Compare two of them to tell which file is newer; render one with
`civil()` from `proc/time.h`, which `date` and `ls -l` both use. `touch_path`
moves a file's mtime to now and is the only thing that can — there is no setter
in OPFS, so a store that cannot be made to restamp answers `Unsupported`.

`kind` is `SYS_KIND_FILE`, `SYS_KIND_DIR` or `SYS_KIND_LINK`. Everything above
that names a path **follows a symbolic link**, except `remove_path`, which drops
the link rather than what it points at, and `read_link`, which is the only way
to see a target. `stat_of(path, false)` reports a final link itself — that is
how `test -h` is written, and the only way to stat one that dangles. A listing
never resolves, so a `DirEntry` says `SYS_KIND_LINK` whatever the link points
at: walk a tree on `SYS_KIND_DIR` alone and it cannot follow a link out of the
tree, which is why neither `ls -R` nor the shell's globber needs a cycle guard.
`make_link` stores the target as written, so it may dangle and a relative one
reads against the directory the link is in.

`tty_of(SYS_STDOUT)` is how a program lays its output out: it reports whether
that descriptor is the terminal and, if it is, how wide. The geometry is zero
for a pipe or a file, so a program that formats for a terminal falls back to one
item per line rather than inventing a width. `/bin/ls` is the worked example.

### `proc/time.h` — the calendar

`civil(secs)` turns seconds since the epoch into a `Civil{year, month, day,
hour, min, sec, weekday}`, with `TIME_MONTHS` and `TIME_DAYS` beside it for the
names. Pure — no syscall — so the wall clock and the timezone are the caller's
to fetch with `clock_now()` and add in. `date` and `ls -l` are the callers.

### `proc/opt.h` — the command line

`OptParse` reads bundled short flags (`-lR`), `--` to end them, and a flag that
takes a value (`-n5` or `-n 5`). It allocates nothing, and options end at the
first operand.

```cpp
constexpr Opts SPEC{ "lr", "n" }; // letters taken; those consuming a value
OptParse opts(args, SPEC);
Opt o;
for (;;) {
    Result<bool> more = opts.next(o);
    if (more.is_err())      // Invalid: an unknown letter, named in o.name
        co_return 2;        // NotFound: a valued letter with nothing after it
    if (!more.value())
        break;
    ...
}
Args paths = opts.rest();
```

Two rules about descriptors, both of which the kernel enforces rather than
trusts:

- **A descriptor named in a spawn is *moved* into the child.** That is what
  closes this side of a pipe, and therefore what lets the other side see an end
  of input. It must not be used after the spawn.
- **One user of a descriptor in one direction at a time.** A second concurrent
  read of the same descriptor is `Err(Perm)`.

A process may have up to four syscalls outstanding at once, across as many tasks
as it has; `proc_spawn(Task<i32>)` starts a second task, and the process ends
when the *root* task returns, whatever the others are doing.

### `proc/screen.h` and `ui/` — painting

A full-screen program claims the alternate screen and the keyboard, paints into
a `Grid` of its own, and sends the damage across in one syscall per frame:

```cpp
ProcScreen s;
co_await s.take_screen();
co_await s.take_keys();
Pane body = s.body();
body.write_at(0, 0, "hello");
co_await s.flush();
Key k = co_await s.next_key();
```

`Pane` clips, never wraps and never scrolls; `TextBuf` holds lines and
`TextView` scrolls a window over one. That is what `less` and `edit` are built
out of, and it links into the binary rather than living in the kernel. A resize
rides on every key reply, so there is no event to subscribe to.

Nothing gives a claim back on your behalf — but nothing has to: a process that
dies has its claims released by the kernel, because a killed program runs no
destructor.

### What the headers do *not* contain

`include/braam/kernel/` and `include/braam/fs/` are shipped because the two
libraries' headers include them, and they are worth reading — `str.h`,
`string.h`, `vec.h`, `span.h`, `result.h`, `fmt.h`, `text.h`, `path.h` are the
whole standard library here. But the parts of them that name the scheduler, the
host imports or the VFS belong to the kernel and have nothing behind them in a
program: reaching one is a link error, which is the intended answer.

---

## 7. The rules that bite

These come from Concept.md §2 and §C.3, and each of them is a compile error, a
link error or a trap rather than a warning:

- **No exceptions and no RTTI.** Errors are `Result<T, E>`.
- **No libc.** No `malloc`, no `memcpy` you did not write, no `<cstring>`.
  `-nostdlib -nostdinc++` is not negotiable, and a construct needing a
  compiler-rt builtin — 128-bit division, an outlined `memcpy` — will not link.
- **Never `new` anything.** `operator new` returns null on failure and there are
  no exceptions, so the expression would construct at address zero. Use
  `heap_new` and `heap_delete` from `kernel/alloc.h`.
- **A namespace-scope global must be trivially destructible.** A non-trivial
  destructor pulls in `__cxa_atexit`, which nothing provides. Make the state a
  POD, or put it behind a pointer built on first use.
- **Keep coroutine frames small.** A frame past 512 bytes costs a whole 64 KiB
  span from the allocator's top size class. Long-lived state belongs in a heap
  block the frame points at, not in the frame.
- **The memory cap is 16 MB**, and it is the kernel's number, not the binary's:
  `--import-memory` with no declared maximum means the host supplies the
  `Memory` and its ceiling.

---

## 8. The worker

**Your program runs in a Web Worker of its own**, and `braam_add_program`
arranges that with nothing asked of you. It is what every program in `/bin`
gets, `/bin/sh` included. What it buys is `worker.terminate()`: a kill that does
not need the program's cooperation, so a bug that loops for ever costs a command
rather than the session.

The cost is that every syscall becomes two `postMessage` hops rather than a call
— 34–45 µs measured, paid per `SYS_CHUNK` — so a program that reads a large file
pays it per 512 bytes, and one being typed into pays it per round trip its
editor makes. There is no way to opt out and nothing to opt out to: a program is
a worker, and the answer to a program that costs too much in round trips is to
make fewer of them. The shell was the last to ask for an exception, and cutting
its round trips was the cheaper answer than weakening its isolation.

You do not have to handle the case where the host has no workers to give,
either. Your program simply has not started yet: the kernel backs off and asks
again — 10 ms, then 20, up to a second — printing `no worker, retrying` on its
stderr, and starts it the moment one can be had. There is nothing to detect and
no degraded mode to write for.

---

## 9. Versioning

The stamp carries a process-ABI number, and the kernel checks it before it runs
anything. `stamp.py` reads that number out of the `kernel/sysabi.h` the SDK
shipped, so a binary is stamped with the ABI of the headers it was actually
built against — never a restated constant that could fall behind.

That is what makes a mismatch a sentence rather than a crash:

```
$ ./hello
sh: hello: built for another process ABI
```

The answer is to rebuild against the SDK that matches the system.
`not executable` is the other one, and it means the file has no stamp at all and
no `#!` line either — an ordinary `.wasm` from somewhere else, a stamp that was
stripped, or a script whose interpreter is missing.

The ABI changes when the syscall table does, and both are documented in
Concept.md §4.3.

---

## 10. Checking a binary by hand

The build produces a module with an exact surface, and it is worth knowing what
it is, because a link that accidentally pulled in kernel code shows up here
first:

- **Imports** are `env.memory`, `kernel.sys` and `kernel.sys_async` — and
  nothing else. Any other import means the process ABI has been gone around.
  `sys_async` is absent from a program that never awaits.
- **Exports** are exactly `_alloc`, `_free`, `_resume`, `_start`. `memory` is
  *imported*, not exported, which is what makes the cap the kernel's.
- **One custom section named `braam`**, five little-endian `u32`s: magic
  `0x6d617262`, the ABI, flags, the initial pages and the maximum.

```
$ node -e 'const m=new WebAssembly.Module(require("fs").readFileSync("build/hello.wasm"));
  console.log(WebAssembly.Module.imports(m).map(i=>i.module+"."+i.name));
  console.log(WebAssembly.Module.exports(m).map(e=>e.name));
  console.log(new Uint32Array(WebAssembly.Module.customSections(m,"braam")[0]))'
```

In the Braam source tree, `test/run.mjs` asserts all of that for every binary,
and will do it for a binary of yours if you hand it one:

```
node test/run.mjs --kernel build/kernel.wasm build/web/rootfs.zip /path/to/hello.wasm
```

---

## 11. Where to read next

- [System_Calls.md](System_Calls.md) — the mechanism end to end: the deferred
  step, the staging protocol, cancellation, the kill, and the whole syscall
  table in one place.
- [Concept.md](Concept.md) — the specification. §4.3 is the process ABI, §4.4 is
  what a process costs, §4.5 is the shell's language, §2 is the three invariants
  everything else follows from.
- [Shell.md](Shell.md) — the shell as a user sees it, which is the long form of
  §4 above: the grammar, the expansions, the twenty-six builtins, the jobs and
  what is deliberately absent.
- `src/cmd/` in the source tree — thirty-six worked examples, from `true.cpp` at
  six lines to the shell.

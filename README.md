# BRAAM - Browser Runtime As A Machine

An operating system that runs in a browser tab.

Braam is a small command-line system: a kernel, a filesystem, a terminal, a shell, thirty-two
programs and six shell builtins. It is written from scratch in C++20 and compiled to WebAssembly,
the binary format browsers run at close to native speed. There is no server side. The whole system
is a handful of static files, so any web host can serve it.

Nothing is borrowed. There is no C library, no Emscripten runtime, no `xterm.js`; every part was
written for this project. The kernel is 136 KB and contains no programs at all. Each program is a
separate WebAssembly file that runs in a sandbox of its own.

Open the page and there is a prompt:

```
$ ls /bin                            # every program, one wasm file each
$ echo hello > notes                 # files under /home survive a reload
$ curl https://www.rfc-editor.org/rfc/rfc2324.txt | less   # a download, into a pager
$ edit notes                         # ^S saves, ^Q quits
$ tail -n 5 /README | wc
$ spin &                             # a program that loops forever
$ kill %1                            # killed anyway
```

It is not a Unix clone. There is no POSIX compatibility layer, no `fork`, no VT100 escape codes,
and no attempt to run C programs written for other systems. Giving that up makes the system about
ten times smaller, and lets every part use what the browser already provides instead of imitating
Unix.

## Three ideas

**A program is a coroutine, and the browser schedules it.** A coroutine is a function that can
pause in the middle and continue later. Anything that would wait pauses instead of blocking, and
the browser continues it when the answer arrives. So there are no threads.

**The terminal is a grid of cells, not a stream of bytes.** A traditional terminal receives text
with escape codes mixed into it and has to interpret them. Here the screen is a two-dimensional
array of cells in memory, so a colour is a field in a cell and moving the cursor is indexing the
array.

**JavaScript never hands data back directly.** When the kernel asks the browser for something, the
answer arrives later through a single callback. That keeps the boundary between the two small
enough to check by eye, and a test asserts exactly what crosses it.

## What is in it

**A shell.** Line editing with history, and one pipeline per line: `|`, `<`, `>`, `>>`, `2>`,
`2>>`, and `&` to run in the background. `^C` stops whatever is running and gives the prompt back.
Background jobs are managed with `jobs`, `fg` and `kill`. `Shift+PageUp` and `Shift+PageDown` page
back over what has scrolled off the screen, and any other key returns to the prompt.

**A filesystem.** `/` lives in memory. `/bin` and `/share` come out of an archive downloaded
alongside the kernel. `/home` is stored by the browser and is the only place where files survive a
reload; `df` reports how much space the browser grants and how much is used. `/proc` shows what is
running. Where the browser will not store files, the system boots with memory only and says so.

**Access to the browser.** Fetching a URL, WebSockets, the clipboard, the file picker, saving a
file, and the clock. So `curl` fetches a URL, `chat` talks between two tabs, `import` and `save`
move files in and out of the browser, and `pbcopy`/`pbpaste` reach the system clipboard.

**Full-screen programs.** `less` and `edit` are built on a layout layer over the grid of cells.
They draw into a grid of their own and send only the part that changed, and `^C` still reaches
them.

**Every program is an isolated process.** No program lives inside the kernel, and there is no way
to write one that does. Each of the thirty-two commands is a separate wasm file that runs in a
worker of its own, with its own memory, its own open files and its own permissions. A program
stuck in a loop is killed outright, without having to cooperate; `spin` exists to show that.

**The shell is one of those programs.** `/bin/sh` is an ordinary binary, and everything a prompt
needs it asks for through the same system calls any program can use. Six commands are *not*
programs, because each one changes the shell's own state: `cd`, `jobs`, `fg`, `kill`, `exit` and
`help`. They are built into the shell, but they are still ordinary pipeline stages, so
`help | grep ls` works.

**An embedding API.** `web/braam.js` puts a terminal on any web page with `mount({ canvas })`, and
`web/embed.html` is a working example.

## Building

You need a clang that can target wasm32, plus CMake, make and Node. On macOS that is
`brew install llvm lld`; on Debian or Ubuntu, `apt install clang lld llvm`.

```
make            # build the kernel, the programs and the tests
make run        # run the tests
make serve      # serve the site and open it in a browser
make install    # install the SDK, to /usr/local or ~/.local
make release    # pack the site and the SDK as build/*.zip
make clean
```

The build leaves a complete website in `build/web/`. It needs no server program and no special
headers, so copying that directory to a web host is a deployment.

## Writing a program

Every command is a wasm file, and nothing about building one is private to this repository.
`make install` puts an SDK under `/usr/local` or `~/.local`, and `make release` packs the same
files as a zip that can be unpacked anywhere.

```cpp
#include "proc/io.h"

Task<i32> proc_main(Args)
{
    co_await write_all(SYS_STDOUT, "Hello, world!\n");
    co_return 0;
}
```

```cmake
find_package(braam REQUIRED)
braam_add_program(NAME hello SOURCES hello.cpp)
```

The result is a 6 KB wasm file. The `Args` parameter has no name because this program ignores it,
and the build treats an unused named parameter as an error.

A program does not have to be part of the system image to run. Paths are looked up through the
ordinary filesystem, so you can bring the file in with the browser's file picker and run
`/import/hello.wasm`, or `curl` it into `/home` and run it there.
[doc/Programming_Manual.md](doc/Programming_Manual.md) is the guide, and
[examples/hello/](examples/hello/) is the worked example the SDK installs.

## Layout

| Directory | What is in it |
| --- | --- |
| [src/kernel/](src/kernel/) | coroutines, memory allocator, scheduler, screen, the JavaScript boundary |
| [src/fs/](src/fs/) | paths, the mount table, the three filesystems |
| [src/svc/](src/svc/) | fetching URLs, WebSockets, clipboard, file transfer, clock |
| [src/ui/](src/ui/) | the layout layer over the grid of cells |
| [src/user/](src/user/) | starting programs, the system calls, the console, pipes, `/proc`, boot |
| [src/proc/](src/proc/) | the runtime that every program carries |
| [src/cmd/](src/cmd/) | one file per program, `sh` among them |
| [src/cmd/sh/](src/cmd/sh/) | the shell |
| [web/](web/) | the page, the workers, the renderer, the browser side of every interface |
| [test/](test/) | the tests, and a simulated browser to run them against |

## Documentation

- [doc/Concept.md](doc/Concept.md) is the specification: what the system is, and the reasoning
  behind each decision. Read it first.
- [doc/Release_Notes.md](doc/Release_Notes.md) says why the code looks the way it does.
- [doc/System_Calls.md](doc/System_Calls.md) walks through how a program talks to the kernel.
- [doc/Programming_Manual.md](doc/Programming_Manual.md) is for writing a program of your own,
  outside this repository.

## Status

Finished, as a first version: everything above works and the tests pass. The kernel is 136 KB and
the archive holding the programs is 204 KB, unpacked into browser storage the first time the page
is opened.

A tablet works: tap to type, drag to select. The row of buttons under the terminal supplies the
keys a touch keyboard does not have, since `Esc`, `Tab`, `Ctrl` and the arrows are not on one.

Some things are missing on purpose. A program cannot be suspended and resumed, so there is no `bg`
and no `^Z`. Lines are not re-wrapped when the window is resized. A process cannot be confined to
its own part of the filesystem. There is no window manager: one program has the screen at a time.
And there are no CPU limits — a program can be killed, but not slowed down.

## License

MIT. See [LICENSE](LICENSE).

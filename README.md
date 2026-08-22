# BRAAM - Browser Runtime As A Machine

An operating system that runs in a browser tab.

Braam is a small command-line system: a kernel, a filesystem, a terminal, a
shell, thirty-nine programs and a package manager. It is written from scratch in
C++20 and compiled to WebAssembly, which browsers run at close to native speed.
There is no server side. The whole system is a few static files, so any web host
can serve it.

Nothing is borrowed — no C library, no Emscripten, no `xterm.js`. Every program
is a separate WebAssembly file running in a sandbox of its own, and none of them
lives inside the kernel.

Open the page and there is a prompt:

```
$ ls /bin                            # every program, one wasm file each
$ echo hello > notes                 # your files survive a reload
$ curl https://example.com | less    # a download, into a pager
$ edit notes                         # ^S saves, ^Q quits
$ spin &                             # a program that loops forever
$ kill %1                            # killed anyway
```

It is not a Unix clone: no POSIX layer, no `fork`, no VT100 escape codes, and no
attempt to run C programs written for other systems. Giving that up makes the
system far smaller and lets every part use what the browser already provides.

## Three ideas

**Programs pause instead of blocking.** Anything that would wait pauses, and the
browser continues it when the answer arrives. So there are no threads.

**The screen is a grid of cells, not a stream of bytes.** A colour is a field in
a cell and moving the cursor is indexing an array, so there is nothing to parse.

**JavaScript never hands data straight back.** Answers arrive later through one
callback, which keeps the boundary small enough to check by eye.

## What is in it

**A shell.** A Bourne shell after v7: variables, functions, `if`, loops, `case`,
globbing, command substitution, pipes, redirection, background jobs, and line
editing with history. `^C` stops whatever is running and gives the prompt back.
[doc/Shell.md](doc/Shell.md) is the manual.

**A filesystem.** Everything lives in the browser's private storage and survives
a reload; `/tmp` is emptied at boot. `/bin` and `/etc` come from an archive
downloaded with the kernel, and are replaced whenever a new version is opened.
`/proc` shows what is running. Where the browser will not store files at all,
the system says so and stops.

**Access to the browser.** `curl` fetches a URL, `chat` talks over a WebSocket,
`fimport` and `fexport` move files in and out, and `pbcopy` and `pbpaste` reach
the clipboard.

**Full-screen programs.** `less` and `edit` draw into a grid of their own and
send only the part that changed. `^C` still reaches them.

**Isolated processes.** Each command runs in a worker of its own, with its own
memory and its own open files. One stuck in a loop is killed outright, without
having to cooperate — `spin` exists to show that. The shell is one of these
programs, not part of the kernel.

**A package manager.** `pkg` installs software that did not ship with the
system. A repository is just files on a web server: an index and one zip per
package. The index is signed, and the keys it is checked against ship inside the
archive. An install is committed by renaming a single symbolic link, so a tab
that dies partway has installed nothing. There is no public repository yet;
[doc/Package_Formats.md](doc/Package_Formats.md) §10 explains running one.

**An embedding API.** `web/braam.js` puts a terminal on any web page with
`mount({ canvas })`, and `web/embed.html` is a working example.

## Building

You need a clang that can target wasm32, plus CMake, make and Node. On macOS
that is `brew install llvm lld`; on Debian or Ubuntu,
`apt install clang lld llvm`.

```
make            # build the kernel, the programs and the tests
make run        # run the tests
make serve      # serve the site and open it in a browser
make install    # install the SDK, to /usr/local or ~/.local
make release    # pack the site and the SDK as build/*.zip
make clean
```

The build leaves a complete website in `build/web/`. It needs no server program
and no special headers, so copying that directory to a web host is a deployment.

## Writing a program

Every command is a wasm file, and building one needs nothing private to this
repository — `make install` puts an SDK under `/usr/local` or `~/.local`.

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

A program does not have to be part of the system to run: bring the file in with
the browser's file picker and run `/import/hello.wasm`, or `curl` it into
`/home` and run it there. [doc/Programming_Manual.md](doc/Programming_Manual.md)
is the guide and [examples/hello/](examples/hello/) is the worked example.

## Layout

| Directory | What is in it |
| --- | --- |
| [src/kernel/](src/kernel/) | coroutines, allocator, scheduler, screen, the JavaScript boundary |
| [src/fs/](src/fs/) | paths, the mount table, the filesystem over browser storage |
| [src/svc/](src/svc/) | URLs, WebSockets, clipboard, file transfer, clock |
| [src/ui/](src/ui/) | the layout layer over the grid of cells |
| [src/user/](src/user/) | starting programs, system calls, the console, pipes, `/proc`, boot |
| [src/proc/](src/proc/) | the runtime every program carries |
| [src/cmd/](src/cmd/) | one file per program; `sh/` and `pkg/` have directories |
| [web/](web/) | the page, the workers, the renderer, the browser side of everything |
| [rootfs/](rootfs/) | what the boot archive carries |
| [tools/](tools/) | the build's scripts, and the ones a package publisher signs with |
| [test/](test/) | the tests, and a simulated browser to run them against |
| [doc/](doc/) | the specification, the manuals and the release notes |

## Documentation

- [doc/Concept.md](doc/Concept.md) — the specification, and the reasoning behind
  each decision. Read it first.
- [doc/Release_Notes.md](doc/Release_Notes.md) — why the code looks the way it
  does.
- [doc/Shell.md](doc/Shell.md) — the manual for `/bin/sh`.
- [doc/System_Calls.md](doc/System_Calls.md) — how a program talks to the
  kernel.
- [doc/Programming_Manual.md](doc/Programming_Manual.md) — writing a program of
  your own.
- [doc/Package_Management.md](doc/Package_Management.md) and
  [doc/Package_Formats.md](doc/Package_Formats.md) — packages: the policy, the
  formats, and how to run a repository.
- [doc/Testing.md](doc/Testing.md) — how the two test suites are organised.

## Status

Version 0.4. Everything above works and the tests pass. A tablet works too: tap
to type, drag to select, with a row of buttons for the keys a touch keyboard
does not have.

Some things are missing on purpose. There is no `bg` and no `^Z`. Lines are not
re-wrapped when the window is resized. One program has the screen at a time.
There are no file permissions and no CPU limits — a program can be killed, but
not slowed down.

## License

MIT. See [LICENSE](LICENSE).

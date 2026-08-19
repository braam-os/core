# Braam — System Calls

How a user process talks to the kernel: the principles, the wire, and what
actually happens on the way through. [Concept.md](Concept.md) is the
specification and this document does not replace it — §4.3 there fixes the ABI
and says why it has the shape it has. This is the walkthrough: the same
mechanism written down end to end, in the order a reader meets it, with the code
beside it.

[Release_Notes.md](Release_Notes.md) holds the arguments. Where a decision here
looks arbitrary, the reason is under M8, M9 or "One program model", and this
document points at it rather than restating it.

A bare `§N` below is always a section of Concept.md, never of this document.

---

## 1. What a system call is here

Braam is a small operating system that runs in a browser tab, and its process
model is the one POSIX taught us: a program is a separate address space, it gets
argv and three standard streams, it opens descriptors, it can be killed, and it
exits with a status. Every one of those nouns survives. What changes is the
mechanism underneath each, because the machine is a browser and not a CPU with a
memory management unit.

| POSIX | Braam | Why |
|---|---|---|
| `fork` then `exec` | one `exec` that instantiates, or one `Sys::Spawn` | there is nothing to copy: a `WebAssembly.Instance` starts empty |
| `waitpid` | `Sys::Wait`, on one child or on any | the status is kept on the parent's record; the scheduler discards a task's return value |
| `pipe(2)` | `Sys::Pipe`, a `Channel<String>` behind two descriptors | §3.6's one pipe type, the same one a shell pipeline is made of |
| `dup2` then `close` in the child | the fd is *moved* into the child by the spawn | one end, one owner: a `Channel` panics on a second blocked sender |
| a page table per process | a `WebAssembly.Memory` per process | a wasm pointer is an offset, not an address; there is nothing to forge (§4.1) |
| `setrlimit` / cgroups | `new WebAssembly.Memory({initial, maximum})` | `memory.grow` simply fails past the ceiling the *host* chose (§4.3) |
| a blocking `read(2)` | `co_await` on a suspension | nothing blocks; the browser event loop is the scheduler (§2.1) |
| a trap into ring 0 | a call to an imported function | an instance can only call the imports it was given, which is the capability system (§4.1) |
| `errno` | a negated `Error` in the reply's leading `i32` | `_resume` has room for a buffer and not for an errno (§4.3) |
| `SIGKILL` | `worker.terminate()` | wasm cannot be preempted; a thread can be ended (§4.2) |
| a core dump | a wasm trap the host reports as exit 132 | a process has no import to log through |
| the file descriptor table | a `Vec<Handle *>` on the kernel's process record | a number one process holds means nothing in another |

Two things POSIX has that Braam does not, and both are deliberate rather than
pending:

**No preemption.** `while(1){}` cannot be interrupted — nothing in the wasm
specification allows it. §4.2 answers this with a kill rather than a scheduler:
a process gets a worker of its own, and `terminate()` ends it without its
cooperation. Bounding CPU rather than ending it would need fuel injection, which
was considered and not built.

**No per-process namespace.** A process has a working directory of its own,
inherited from whoever spawned it, but there is no per-process *root*: once a
path is absolute, `open` resolves it with the kernel's full authority. `cd` is
still a shell builtin, because the directory it moves is the shell process's own
— the one a command typed at the prompt inherits at spawn — and no syscall may
reach another process's anything. A `/bin/cd` would move its own and exit.

---

## 2. The three participants

Only JavaScript can call a `WebAssembly.Instance`'s exports. Two instances
cannot reach each other, and there is no instruction that would let them try. So
every crossing goes through the host, and the host's scheduling discipline is
the first thing to understand.

```
┌───────────────────── Web Worker: the kernel's ─────────────────────┐
│                                                                    │
│  kernel.wasm                            web/proc.js                │
│  ┌──────────────────────┐               ┌───────────────────────┐  │
│  │ proc_spawn ──────────┼── host_svc ──►│ makeProc: compile,    │  │
│  │ proc_step ───────────┼── host_svc ──►│   hire, bind, post    │  │
│  │ proc_kill ───────────┼── host_svc ──►│   a step, terminate   │  │
│  │                      │◄── wake(tok) ─│                       │  │
│  │ sys(pid,op,…)        │◄── Stage,Exit─│ and unpack a reply,   │  │
│  │ sys_async(pid,op,…)  │◄── the calls ─│   which is where the  │  │
│  │                      │               │   syscalls arrive     │  │
│  └──────────────────────┘               └───────────────────────┘  │
│                                                                    │
└───────────────────────────────┬────────────────────────────────────┘
                                │ postMessage
                                │   down: bind, step
                                │   up:   the result, the exit
                                │         status, the calls made
┌───────────────────────────────┴────────────────────────────────────┐
│                                                                    │
│  the binary, e.g. /bin/echo             web/proc.js                │
│  ┌──────────────────────┐               ┌───────────────────────┐  │
│  │ exports              │               │ serveProc: one        │  │
│  │  _start(ptr,len)     │◄── a step ────│   Instance, and the   │  │
│  │  _resume(tok,ptr,len)│◄── a step ────│   two calls it can    │  │
│  │  _alloc(n) _free(p,n)│◄── the copy ──│   make                │  │
│  │                      │               │                       │  │
│  │ imports              │               │ workerOps(pid):       │  │
│  │  env.memory          │               │   exit, getpid, now   │  │
│  │  sys(op,a0,a1,a2)    │──────────────►│   answered on the     │  │
│  │  sys_async(op,t,p,n) │──────────────►│   spot, the rest ride │  │
│  │                      │               │   the reply back up   │  │
│  └──────────────────────┘               └───────────────────────┘  │
└────────────────────────────────────────────────────────────────────┘
```

**The rule everything follows: the kernel never calls a process, and the host
never calls one while the kernel is on the stack.** The first half is not a
choice — wasm has no instruction that reaches another instance. The second half
is, and it is the load-bearing one.

The host *could* call `_start` from inside `host_svc`, the way `test/fakefs.mjs`
answers a storage request from inside the import. That works there because a
storage reply only queues a resumption. A process step is not a reply: it runs a
program, and that program immediately calls back in through `sys`, which
allocates, touches the process table and wakes a token. Doing that on top of a
half-finished `HostCall::issue()` corrupts the kernel's heap in a way that
surfaces weeks later.

So one `_start` or `_resume` is a **deferred host action**, structurally
identical to a storage reply: the proxy task parks on a wake token, the host
steps the instance once the tick has unwound, and the token is woken with the
outcome. The deferral is the `postMessage` itself, which is deferred by nature;
in the test driver it is the link being pumped between ticks
(`test/fakeworker.mjs`). The stepping code is the same `web/proc.js` in both,
because the difference is who carries the message and not what it says.

Synchronous syscalls run the other way and need none of this. `sys(pid, …)`
re-enters the kernel from JS at top level, exactly as `key()` and `wake()` do,
and answers without parking.

---

## 3. Five principles

**Nothing blocks, so a syscall is a suspension.** A process's `co_await`
suspends its coroutine, its runtime returns control out through
`_start`/`_resume`, the kernel continues doing other work, and the host calls
`_resume` later with the payload. That is reentrant scheduling across an
instance boundary with no stack switching, no Asyncify and no JSPI. The
process's scheduler is two arrays (`src/proc/rt.cpp:26-33`); the kernel's is the
real one.

**A step is queued, never nested.** The rule stated under "The three
participants" above, and the consequence a reader trips over: a syscall's
*answer* is not produced by the call that asked for it. `sys_async` records a
request and returns; a scheduler job of the kernel's performs the work; a later
step delivers the result.

**The pid is written into the closure, not passed.** A process's two imports are
built per instantiation with its pid baked in — `workerOps(pid)` at
`web/proc.js:133`, closed over by the `kernel: {…}` import object at `:72`. The
kernel's exports take a pid the *host* supplies; the process's imports have no
argument for one. "A process cannot issue a syscall on behalf of another PID" is
therefore not a check that runs, it is a shape the ABI has, and
`test/run.mjs:206` asserts the shape.

**What crosses is bytes, not addresses.** Two instances have two memories, so
every transfer is a copy through the host (Appendix B). The kernel cannot be
handed a buffer it did not allocate, so the host asks for one first:
`Sys::Stage` is a synchronous syscall the *host* issues on the process's behalf,
returning the address of a staging block the process's kernel-side record owns.
The reverse direction needs no such call, because `_alloc` is already in the
ABI.

**One import per calling convention, so the table grows by an enum value.** The
kernel has six host imports and gained none for processes: spawning, stepping
and killing are three more operations on `host_svc`, which is what that import's
convention already was. The same discipline governs the syscall table — adding
an operation is a number on each side, never a new function.

---

## 4. The wire

§4.3's block, which both ends include as `src/kernel/sysabi.h`:

```
process imports:  env.memory                        // the kernel's, so the cap is the kernel's
                  sys(op, a0, a1, a2) -> i32        // sync ops, immediate result
                  sys_async(op, token, ptr, len)    // async ops, reply via _resume

process exports:  _start(argv_ptr, argv_len) -> i32 // 0 = exited, 1 = suspended
                  _resume(token, ptr, len)   -> i32 // the same
                  _alloc(n) -> ptr, _free(ptr, n)

custom section "braam":  magic, abi, flags, initial_pages, max_pages
```

The kernel's half is two exports with a leading pid:

```c
BRAAM_EXPORT("sys")       i32 sys(u32 pid, u32 op, u32 a0, u32 a1, u32 a2);
BRAAM_EXPORT("sys_async") i32 sys_async(u32 pid, u32 op, u32 token, u32 len);
```

The extra `pid` argument *is* the capability boundary, and the missing `ptr` on
the kernel side is the address-space boundary: the pointer the process passed is
into the process's memory, so the host consumes it during the copy and never
forwards it.

### 4.1 The op word

Only the asynchronous half packs an argument. The low byte is the operation; the
upper 24 bits are its one immediate — a descriptor at `Write`/`Read`/`Close`,
the open flags at `Open`, a single bit elsewhere:

```c
inline u32 sys_op(Sys op, u32 arg = 0) { return u32(op) | (arg << 8); }
inline Sys sys_op_code(u32 op)         { return Sys(op & 0xff); }
inline u32 sys_op_arg(u32 op)          { return op >> 8; }
inline u32 sys_op_fd(u32 op)           { return sys_op_arg(op); }
```

One convention rather than two, so a payload is only ever the operation's
*data*: a write hands over its bytes and an open hands over its path, neither
with a header glued on the front. `sys_op_fd` is the same accessor renamed where
the argument is a descriptor.

The synchronous half does not pack anything — `sys` has three spare scalars, so
`Sys::Exit` puts its status in `a0` and the others take none.

### 4.2 The reply payload

`_resume`'s signature has room for a buffer and not for an errno, and every
asynchronous syscall needs both. So **a reply is an `i32` status followed by any
data**:

```
   0        4                                                    len
   ┌────────┬──────────────────────────────────────────────────────┐
   │ status │ the operation's data, if it has any                  │
   └────────┴──────────────────────────────────────────────────────┘
```

A negative status is `Error(-status)` from `src/kernel/result.h`; zero or above
is the operation's answer — bytes written, a descriptor, or nothing. The decode
happens in exactly one place, `SysCall::await_resume`
(`src/proc/rt.cpp:109-121`), so every wrapper in `src/proc/io.h` just propagates
a `Result`.

A reply shorter than four bytes is synthesised as `-Error::Io`: a truncated
reply is a broken host, and guessing would be worse.

### 4.3 argv

argv crosses an address space, so it is one blob rather than a pointer array:
`u32 argc`, then a length and bytes per word (`sysabi.h:220-272`). The host
allocates room for it inside the process with `_alloc` — which is what `_alloc`
is for, and why `argc` alone could not have said where the blob went — writes
it, and passes the pointer to `_start`.

The blob is deliberately never freed. `Args` is a span of `Str` views into it,
and a program may hold those until it exits.

### 4.4 The metadata

`exec` has to know two things before it can instantiate a binary: where to run
it, and how much memory to give it. It lives in a wasm custom section named
`braam`, five little-endian `u32`s appended after the link by `tools/stamp.py`:

```c
struct ProcMeta {
    u32 magic;          // 0x6d617262, "bram"
    u32 abi;            // PROC_ABI, currently 10
    u32 flags;
    u32 initial_pages;
    u32 max_pages;
};
```

Stamped after the link rather than compiled in, because `initial_pages` must
agree with `-Wl,--initial-memory` and the four lines of `src/cmd/CMakeLists.txt`
that set the link flag are the one place that knows both. `strip()` drops any
earlier section of the same name, so stamping twice is stamping once.

`exec_meta` walks the section list and refuses a binary whose magic or `abi` is
not the kernel's. That is what the `abi` word is for: an ABI amendment makes a
stale binary a diagnostic rather than a wrong answer. The two refusals are
**different errors**, because they call for different repairs: `Err(Invalid)` is
a file that was never a program — no `braam` section, or bytes that are not a
module — while `Err(Unsupported)` is a section of ours carrying somebody else's
number, which is a stale binary and wants saying so. `exec_resolve` propagates
both, so a typed command reads `<name>: built for another process ABI` and a
`/bin/sh` that will not resolve names the number this kernel speaks.

`Err(Invalid)` now means a file that was never a program **and had no `#!` line
either**. A file that is not a module is looked at once more by `exec_shebang`,
and a first line naming an absolute interpreter makes it one: `exec_resolve`
re-resolves to that interpreter — **one level only**, so an interpreter that is
itself a script is `Err(Invalid)` — and instantiates it with the lead words
`[interpreter, argument?, resolved script path]` in place of argv[0].
`Sys::Spawn` still reports one child: the resolution happens before any process
exists, so the depth and child caps count the interpreter as they counted a
binary. An interpreter that is not there folds into `Err(Invalid)` rather than
surfacing as the `Err(NotFound)` of a command that does not exist, so a shell
says `not executable` and 126 for a script that is there and will not run.

Nothing in the section says *where* the process runs, because there is only one
place: a worker of its own (Concept.md §4). The `tier` word that used to sit
third is gone rather than reserved — `abi` is what refuses a binary from a build
that had one.

`proc_pack` folds the two numbers the host needs into the request record's one
spare word:

```
   31                   16 15                     0
   ┌───────────────────────┬───────────────────────┐
   │       max_pages       │     initial_pages     │
   └───────────────────────┴───────────────────────┘
```

`web/proc.js:29-31` mirrors both accessors, as `web/abi.js` mirrors the record
itself.

---

## 5. The synchronous half, and why it is closed

Four operations, answered inside the export, never parking:

| # | Name | Arguments | Returns |
|---|---|---|---|
| 1 | `Exit` | `a0` = exit status | 0 |
| 2 | `GetPid` | — | the pid |
| 3 | `Now` | — | milliseconds since boot |
| 4 | `Stage` | `a0` = bytes about to be copied in | a kernel address, or 0 |

`exec_sys` (`src/user/exec.cpp:319-337`) is a plain switch with no scheduling in
it at all. Note that `Sys::Exit` only *records* the status on the process
record; the process still has to return from `_start`/`_resume` before the step
reports `Exited`.

**`Sys::Stage` is the host's syscall, not a program's.** It exists so the host
can ask where to copy a payload, and a program never calls it — but a hostile
binary can, so it is bounded by `SYS_STAGE_MAX` (1 MiB, the largest blit there
can be) rather than handed an arbitrary `heap_alloc`.

**The set is closed at four, permanently.** All four are answerable inside the
process's own worker with no kernel to ask, which is the only reason a
synchronous half exists at all — the boundary a syscall crosses has no
synchronous direction:

- `GetPid` is the constant the host bound into the closure when it made the
  worker.
- `Now` is the clock reading the step message carried plus the worker's own
  elapsed time — monotonic and relative rather than identical to the kernel's
  tick clock, which nothing depends on.
- `Exit` is buffered and rides back on the step's reply, the last one before the
  step returned. A process only ever issues it immediately before returning, so
  nothing observes the delay.
- `Stage` is refused with 0 — the "no room" answer the runtime already handles.
  Unknown operations are refused locally too, and never relayed.

A fifth synchronous operation would have nothing to answer with inside a worker
and would fail there alone, which is the worst way for an ABI to break. So an
operation that needs the kernel is asynchronous whatever it costs.

---

## 6. The asynchronous half, and the two tokens

`sys_async(op, token, ptr, len)` hands over a request and returns immediately.
The process's awaitable then suspends, and the step it is inside of returns 1.

```c
void SysCall::await_suspend(std::coroutine_handle<> h)
{
    Rt &r = rt();
    for (slot_ = 0; slot_ < PROC_TASKS; slot_++)
        if (!r.waiters[slot_].live)
            break;
    ...
    Waiter &w = r.waiters[slot_];
    w.h       = h;
    w.token   = ++r.token;
    w.live    = true;
    sys_async(op_, w.token, proc_addr(payload_.data()), u32(payload_.size()));
}
```

`SysCall` is used as an awaiter *directly* rather than through a `Task`, so no
coroutine frame is allocated for it and the payload is still on the caller's
stack when the host copies it out. `await_ready()` is unconditionally false:
**an asynchronous syscall always costs a park and a step**, even where the
kernel could have answered instantly. That is the cost model to carry into any
program you write.

### Two token namespaces

This is the single easiest thing to misread in the code, because both are called
`token` and they travel together.

The **host-request token** is the kernel's own: `HostCall::await_suspend` takes
one from `sched_token()`, hands it to `host_svc`, and the host answers it with
`wake()`. It names a suspended *kernel* task. Every asynchronous host operation
has one, and a process step is just another of them.

The **process syscall token** is the process's own: `++r.token` above, starting
at 2 because 0 is reserved for `_start` — which answers nothing. It names a
suspended *process* task, and it rides opaquely through the whole loop:

```
   process        kernel                 host                process
   ──────────────────────────────────────────────────────────────────
   ++r.token ──► Call::token ──► Reply::token ──► proc_step(token)
                                                        │
                                          req.flags ────┘
                                                        │
                                     _resume(token, …) ─┴──► matched in
                                                             the waiter table
```

`flags` is where it rides on the step request, because nothing else on a step
uses that word. The kernel names the call it is answering rather than the host
remembering the last one, which is what makes several outstanding calls
representable at all.

---

## 7. What actually happens

### 7.1 `echo hi`, end to end

The stepper is an ordinary scheduler job — `exec_process`, the task the shell's
pipeline stage runs. It never performs a syscall itself. Three participants, and
the trace has a column each: the stepper inside `kernel.wasm`, `makeProc` beside
it in the kernel's worker, and the process's own worker. That last column holds
`serveProc` and the instance together, because between those two a call is a
call and nothing is deferred; §7.3 opens it up.

```
 stepper (kernel)                   makeProc (kernel worker)            the process's worker
 ────────────────                   ────────────────────────            ────────────────────
 exec_resolve("echo")
   read /bin/echo, parse the braam section → 4 pages, cap 256
       │
 proc_spawn ──── host_svc(ProcSpawn) ────►│
       │                                  │ compile (cached by path)
       │                                  │ take() an idle worker, or hire one
       │                                  │ postMessage bind ────────►│
       │◄─────────── wake(tok) ───────────│                           │ new Memory({4, 256})
       │                                  │                           │ new Instance: env.memory,
       │                                  │                           │   sys/sys_async → this pid
       │
       │ the spawn is answered without waiting for that message: an
       │ instance that will not build reads as a process that died
       │ at its first step
       │
 proc_step(token=0, argv) ── host_svc ───►│
       │                                  │ postMessage step ────────►│ ops.begin(now)
       │                                  │ {now, token, argv}        │ _alloc(18), copy argv in
       │                                  │   the buffer is moved,    │ _start(ptr,18) → proc_main
       │                                  │   not copied              │   write_all(1, "hi")
       │                                  │                           │   sys_async(Write|1<<8,
       │                                  │                           │     tok=2, ptr, 2) → calls
       │                                  │                           │ returns 1: suspended
       │                                  │◄── postMessage step ──────│
       │                                  │ {result:1, calls:[{op, token, len, buf}]}
       │◄─── sys(pid, Stage, 2) ──────────│
       │ proc_stage → &Call.stage         │ mem.view().set(bytes, dst)
       │◄ sys_async(pid, Write|1<<8, 2, 2)│
       │ Call{op, len=2, token=2} pushed  │
       │◄─────────── wake(tok) ───────────│
       │
       │ ProcStep::Suspended, and p->calls has one entry with no server:
       │ sched_spawn(serve(p, c)) ───────────────────────────► serve()
       │                                                       │
       │ co_await p->done.recv()         proc_syscall: co_await
       │      ⋮                          p.io.out.write("hi")  │
       │◄──── Reply{token=2, status 2, no data} ──────────────┘
       │
 proc_step(token=2, reply) ─ host_svc ───►│
       │                                  │ postMessage step ────────►│ _alloc(4), copy the reply
       │                                  │ {now, token=2, status 2}  │ _resume(2, ptr, 4)
       │                                  │                           │   await_resume → 2 written
       │                                                              │
       │ …and the whole round trip again for "\n": echo writes        │
       │ its words and its newline separately…                        │
       │                                                              │
       │                                  │                           │ proc_main returns 0
       │                                  │                           │ sys(Exit, 0) buffered
       │                                  │                           │ returns 0: exited
       │                                  │◄── postMessage step ──────│
       │                                  │ {result:0, exit:0, calls:[]}
       │◄─── sys(pid, Exit, 0) ───────────│
       │ p->exit = 0                      │
       │◄─────────── wake(tok) ───────────│
       │
       │ ProcStep::Exited → co_return p->exit  (0)
       │ ~End: proc_kill(pid) → the worker is pooled, the Proc record freed
```

Five things in that trace are worth naming. The `bind` message is not answered,
so the spawn is answered before the instance exists — a worker that will not
load or a module that will not instantiate surfaces at the first step, as a
process that died. The `Sys::Stage` call happens *before* `sys_async`, because
the kernel must own the destination. `_start` returning 1 is the only thing the
kernel learns without a syscall — that is the whole of `status_of`'s return
convention. `Sys::Exit` is issued by the *runtime*, not by the program: `echo`
returned 0 from `proc_main` and `status_of` reported it
(`src/proc/rt.cpp:62-76`) — and it is *buffered* in the worker rather than sent,
because a synchronous call has no way out of one, so it rides the step's reply
and the kernel is told microseconds before it learns the step ended.

And the two-byte write is not a simplification — that is what `echo hi` really
does, one `write_all` per word and another for the newline, each a full park and
step. Nothing coalesces them, which is the cost model of §6 arriving in the
smallest possible program. A filter that reads and writes `SYS_CHUNK` at a time
pays the same overhead per 512 bytes instead of per two.

### 7.2 A synchronous call, inside the worker

`proc_pid()` never leaves the worker the process is in, which is what makes the
synchronous half possible across a boundary that has no synchronous direction:

```
   spin.wasm
     │ sys(GetPid, 0,0,0)
     ▼
   workerOps.sys
     │ return pid   ← bound in at bind time
     ▼
   (no kernel involved, no message sent)
```

That is also why the synchronous half is closed at four (§5): an operation that
needs the kernel has no way to ask for it from here.

A worker boundary has no synchronous direction — §1 rules out
`SharedArrayBuffer` and therefore `Atomics.wait` — and `sys` is by construction
synchronous. It survives because none of its four operations has to reach the
kernel at all. That is the result M9 turned on, and deleting tier 2 is what made
it the only case there is: `spin.wasm` has no other place to run, so those four
are answered in its own worker or not at all.

### 7.3 The step message, both ways

§7.1's third column, opened up. Two messages, one each way, whatever the process
did inside the step — one `_resume`, or a hundred syscalls and a spawn:

```
 kernel worker                                    process worker
 ─────────────                                    ──────────────
 step(r, done)
   postMessage({k:"step", now, token, payload},
               [payload.buffer])  ───────────────►│
                                                  │ ops.begin(now)
                                                  │ server.step(token, payload)
                                                  │   _alloc, copy, _resume ──►│
                                                  │                            │ …runs…
                                                  │◄── sys(Exit, 3) ───────────│  buffered
                                                  │◄── sys_async(Read|0, 5, …) │  pushed onto
                                                  │                            │  `calls`
                                                  │◄──── returns 1 ────────────│
                                                  │ {exit, calls} = ops.end()
   │◄── postMessage({k:"step", result:1, pages:7, │
   │        exit:3, calls:[{op,token,len,buf}]},  │
   │        [each buf]) ──────────────────────────┘
   │
 finish(p, m):
   kernel().sys(pid, Exit, 3)
   for each call:
     dst = kernel().sys(pid, Stage, len)
     mem.view().set(new Uint8Array(call.payload), dst)
     kernel().sys_async(pid, call.op, call.token, call.len)
   pending.r.ok(result, pages); pending.done()   → wake(tok)
```

That `for` loop is the whole kernel side of the protocol: `finish` in
`web/proc.js`, straight-line code, and the only place `Sys::Stage` is ever
called. Both halves of the wire live in that one file — `serveProc` and
`workerOps` are the other end of these same two messages — so that two files
cannot describe one wire.

The exit status goes first and the step's own answer last, which is what leaves
`p->exit` already recorded by the time the stepper wakes to `ProcStep::Exited`
and reads it (§5).

`pages` is how much memory the instance has committed, and it rides here rather
than on an operation of its own: only the worker can read a
`WebAssembly.Memory`, `/proc` is generated with nothing to await, and the step
is already a message each way. It arrives in the reply record's otherwise unused
`result_hi`, and `proc_step` hands it to the stepper through a `u32 *pages`
out-param, which stores it on the `Proc` record for `/proc` to publish as a
process's usage.

`slice` rather than `subarray` is load-bearing on both sides. A view is detached
by the next `memory.grow` (§8.4), and one that has been transferred cannot be
re-derived.

### 7.4 Two calls outstanding

`chat` listens to a socket while it reads what is typed, so it spawns a second
task with `proc_spawn`. Two tasks mean two syscalls parked at once, and that is
what the token and the per-call server job are for:

```
 chat.wasm                     kernel
 ─────────                     ──────
 task 0: read_chunk(STDIN)  ─► Call{op=Read|0, token=2} ─► serve() job A
 task 1: read_chunk(sock)   ─► Call{op=Read|3, token=3} ─► serve() job B
                                                              │
   stepper: co_await p->done.recv()                            │
                                                              │
   ...the socket is quiet for a minute...                      │
                                                              │
 job A completes first ─► Reply{token=2, "hi\n"} ─► done channel
   stepper: proc_step(token=2, …) ─► _resume(2, …) ─► task 0 wakes
```

Both of those had to be fixed for this to work, and each was a real bug waiting:

- **One staging buffer per process would have lost data.** The second
  `Sys::Stage` would have handed back the same block and overwritten the first
  call's payload before its server read it. So the staging block lives on the
  `Call` record (`src/user/proctab.h:164-173`), allocated on demand and promoted
  out of `p->staging` when `sys_async` arrives.
- **One proxy performing calls in turn would have starved them.** A socket read
  that never completes would hold up the keystroke behind it. So the stepper
  spawns a scheduler job per call and parks on a channel, and the jobs finish in
  whatever order the world answers them.

`PROC_TASKS` is 4 on the process side — one waiter each — and the kernel's reply
channel is sized 8 so a completing server never parks on the send.

### 7.5 `^C`, and the kill

A stage is a `Task<i32>` like any other, so `^C` reaches it through exactly the
awaitables it reaches any other awaiting task through. The destructor is the
whole kill path:

```
 ^C → the job's CancelToken is signalled
        │
        ├─► every awaitable the servers are parked on returns Err(Cancelled)
        │     serve() sees Cancelled and returns *without* replying —
        │     the process is going, and nobody is left to hear
        │
        └─► exec_process's frame is destroyed → ~End runs:
              │
              ├─ for each Call with a server:  sched_cancel(server)
              │
              ├─ proc_kill(pid) ── host_svc(ProcKill, 0, pid, null) ──►  host
              │                                                            │
              │    own worker:  link.terminate(), and the part that ───────┤
              │                 is easy to miss: the in-flight step        │
              │                 must be FAILED by whoever killed it        │
              │                 — r.fail(NOTFOUND); done() → wake(tok)     │
              │                                                            │
              │    kernel's:    procs.delete(pid) — a queued step   ───────┘
              │                 will run, find the pid gone, and fail itself
              │
              └─ proc_remove; heap_delete(p) → ~Proc:
                   ~FullScreen  (the screen comes back)
                   ~KeyInput    (the keyboard comes back)
                   every Handle (fds close; ~JsRef drops the externref,
                                 so the socket closes with it)
```

That last line is not tidiness. An abandoned `HostReq` is reaped by `wake()` on
its token and by nothing else, so a request whose worker no longer exists leaks
the record and its payload for the life of the page unless the killer answers
it. Whoever takes the worker away — `kill()`, `dropWorkers()` — is who must fail
the step in it.

`proc_kill` is told, not asked: no record, no reply, the pid in the `req`
position and a null externref, because it is issued from a destructor where
there is nothing left to await with.

A killed process never unwinds. Its coroutine frames, its heap and its
descriptors go at once when the instance is dropped — which is the isolation
working, not a shortcut.

### 7.6 A trap, and the exit statuses

A process has no import to log through, so a fatal error is `__builtin_trap()`.
The host catches the exception, nulls the instance and reports
`ProcStep::Trapped`; the kernel turns that into a status and a message.

| Status | Meaning | Where |
|---|---|---|
| `p->exit` | the process returned from `proc_main` | `exec.cpp:284` |
| 126 | the binary will not instantiate | `exec.cpp:252` |
| 130 | cancelled — `^C`, `kill`, a job going away | `exec.cpp:249,279,312` |
| 132 | trapped | `exec.cpp:289` |
| 1 | a resource failure, or "suspended with nothing pending" | `exec.cpp:306` |

"Suspended with nothing pending" is unrepresentable in a correct runtime — a
step that reports `Suspended` must have parked on something — so it is reported
rather than looped on.

An instance in a worker of its own is created there, so a binary that will not
instantiate reads as 132 rather than 126. The module is still compiled in the
kernel worker, so a malformed one is still refused before anything runs; only
the distinction is lost, and it was not worth an ABI change to keep.

---

## 8. The syscall reference

The opcode is the low byte of the op word. The groups are sparse on purpose, so
one can grow without renumbering anything. Every operation has a caller in
`src/cmd/` — a syscall nothing calls is an ABI nothing tests.

### Synchronous — `sys(op, a0, a1, a2) -> i32`

| # | Name | Argument | Returns |
|---|---|---|---|
| 1 | `Exit` | `a0` = status | 0; recorded, effective when the process returns |
| 2 | `GetPid` | — | the pid |
| 3 | `Now` | — | ms since boot (`sched_now()`) |
| 4 | `Stage` | `a0` = byte count | a kernel address, or 0 for "no room" |

### Asynchronous — `sys_async(op, token, ptr, len)`

Reply is `i32 status` then data. A negative status is `-Error`. Served in
`proc_syscall`, `src/user/syscall.cpp:380-1320`.

| # | Name | Op-word arg | Payload | Status | Data |
|---|---|---|---|---|---|
| 16 | `Write` | fd | the bytes | bytes written | — |
| 17 | `Read` | fd | — | bytes read, 0 at end | the chunk |
| 18 | `Open` | `SYS_O_*` | the path | the fd | — |
| 19 | `Close` | fd | — | 0 | — |
| 20 | `Stat` | — | the path | 0 | `u32 kind`, `u64 size` |
| 21 | `List` | — | the path | 0 | `u32 count`, then per entry `u32 kind`, `u64 size`, `u32 name_len`, the name |
| 22 | `MkDir` | — | the path | 0 | — |
| 23 | `Remove` | bit 0 = recursive | the path | 0 | — |
| 24 | `Chdir` | bit 0 = set, else report | the path, when setting | 0 | the resulting absolute cwd |
| 25 | `Dup` | fd | — | a second fd for the same thing | — |
| 32 | `Sleep` | — | `u32 ms` | 0 | — |
| 48 | `Clock` | — | — | 0 | `u64 epoch_ms`, `i32 tz_min` |
| 49 | `Storage` | — | — | 0 | `u64 quota`, `u64 usage`, `u32 flags` |
| 50 | `Fetch` | — | `u32 url_len`, the url, the spec | the body's fd | `u32 http_status`, the headers |
| 51 | `WsOpen` | — | the url | the socket's fd | — |
| 52 | `ClipRead` | bit 0 = wait for a paste | — | the text's length | the text |
| 53 | `ClipWrite` | — | the text | 0 | — |
| 54 | `Pick` | — | — | the set's fd | `u32 count`, then `u32 len` and a name each |
| 55 | `PickOpen` | the set's fd | `u32 index` | the file's fd | — |
| 56 | `Fexport` | — | `u32 name_len`, the name, the bytes | 0 | — |
| 64 | `KeyClaim` | bit 0 = take, else release | — | 0 | `u32 cols`, `u32 rows` |
| 65 | `KeyRead` | — | — | 0 | `u32 code`, `u32 mods`, `u32 cols`, `u32 rows` |
| 66 | `ScreenEnter` | bit 0 = enter, else leave | — | 0 | `u32 cols`, `u32 rows` |
| 67 | `ScreenBlit` | — | seven `u32`s, then `w*h` `Cell`s | 0 | — |
| 68 | `ScreenClear` | — | — | 0 | — |
| 69 | `Cursor` | bit 0 = set, else report | `u32 x, y, on` when setting | 0 | `u32 x`, `y`, `on`, `cols`, `rows` |
| 70 | `Style` | `fg \| bg << 8 \| attrs << 16` | — | 0 | — |
| 71 | `Echo` | `SYS_ECHO_SHOW \| FRESH \| END` | `u32 x, y, cur, runs`, then `u32 style, len` each, then the bytes | 0 | `u32 x`, `y`, `on`, `cols`, `rows`, `scrolled` |
| 72 | `Tty` | fd | — | 0 | `u32 flags`, `u32 cols`, `u32 rows` |
| 80 | `Pipe` | — | — | 0 | `u32 read fd`, `u32 write fd` |
| 81 | `Spawn` | `SYS_SPAWN_ENV` | `u32 fd0, fd1, fd2`, the argv blob, then the env blob | the child's pid | — |
| 82 | `Wait` | a pid, or `SYS_WAIT_ANY` | — | the child's status, 0–255 | `u32 pid` |
| 83 | `Kill` | the pid | — | 0 | — |
| 84 | `Fg` | a child's pid, or 0 to take the console back | — | 0 | — |

Every multi-byte field is little-endian, and a `u64` is a low word then a high
word.

`Chdir` sits at 24 rather than with the process family because it is the state
`Open`, `Stat`, `List`, `MkDir` and `Remove` resolve *against* — it belongs with
the operations it governs, and a program that never spawns anything still uses
it. `pwd` is its caller.

**`Dup` is the only way to say `2>&1`, and the only way a shell keeps a
descriptor across a `Spawn`.** One handle stands behind both numbers, so a
file's offset is shared — which is what makes `>f 2>&1` interleave rather than
overwrite — and closing one shuts nothing until the last goes. It exists because
`Spawn` *moves*: without it `exec >file` would lose the shell's own descriptor
to the first child that ran.

Adding it moved `PROC_ABI` from 9 to 10 and relaxed one rule in `Spawn`. (The
environment moved it from 10 to 11, without adding an operation: the blob rides
`Spawn`'s payload and `_start`'s.) That operation used to refuse a handle with
`refs > 1`, meaning "nothing this process is inside a syscall on" — a second
*descriptor* raises that count too, so every duplicated fd would have been
unspawnable. The test is now the `busy_r`/`busy_w` flags, which is what the
sentence always meant. Two slots may still not name the same number; `2>&1`
gives them two numbers over one handle instead.

**`Cursor` is the scrolling screen's, not the alternate one's.** `Write` moves
the cursor as a side effect — it goes through the same `screen_write` that wraps
and scrolls — and reports a byte count, so a program that draws a prompt has no
way to know where it ended up. A *set* is refused with `Err(Perm)` while another
process holds the alternate screen, for the reason `ScreenBlit` is; a get is
always allowed. It has **no caller in the tree** any more — `Echo` is what a
prompt uses — and it stays because a program that wants to know where it is has
nothing else to ask.

**`Echo` is those two and a `Write` in one operation**, because a repaint is one
change to the grid and was four round trips. The payload names the anchor and
how many cells past it to leave the cursor; the operation moves the cursor
there, writes the bytes through the same `Stream` `Write` uses, and puts the
cursor `x + cur` cells past the anchor — carried up by whatever the write
scrolled. `scrolled` is that number, and it is what the second `Cursor` call was
for: nothing else counts scrolls, since the grid moves under a write and
`cursor_y` does not change. `screen_scrolled()` is a counter the screen keeps,
and the operation reports the difference across itself, so a resize that drops
rows from the top is folded in the same way a scroll is.

**Its bytes are a sequence of styled runs**, because a prompt is three colours
and a reset and was seven round trips of its own. Every run header comes before
every byte — `runs` of them, each a style word and a length — so the whole shape
is checkable in one bounded pass before a cell moves: the count against
`SYS_ECHO_RUNS_MAX`, the headers against the payload, and the lengths summed in
a `u64` against exactly what is left. Anything else is `Err(Invalid)` and paints
nothing. A run's style is applied even when its length is zero, which is how the
default goes back on; a style of `SYS_STYLE_KEEP` names no colour and leaves the
sticky one standing, which makes that run exactly a `Write`.

**Two bits carry what the `Cursor` gets were for.** `SYS_ECHO_FRESH` anchors
wherever the cursor is, on a row of its own — a newline first unless the cursor
is already in column 0 — so `x` and `y` are ignored. `SYS_ECHO_END` leaves the
cursor where the write ended, off the deferred wrap column, so `cur` is ignored.
Both newlines go out through the `Stream` like every other byte, so a redirected
stdout sees them; and `FRESH`'s goes out *ahead of the first run's style*, which
is why a prompt that scrolls the grid no longer blanks the new bottom row in the
prompt's own colour.

Its refusal and its rules are `Cursor`'s, and it needs no others: everything it
can do, `Cursor`, `Style` and `Write` could already do. What it buys is §4.4's
cost paid once instead of per operation — a keystroke is two round trips where
it was five, and Enter to the next prompt is five where it was twelve — and one
*tick* instead of many, which is why the cursor no longer has to be hidden
through a repaint. The grid is presented at the end of every tick, so a
keystroke painted three times and a prompt seven, with the cursor visibly
walking the line. `/bin/sh` is the caller.

**`Style` is the colour `Write` cannot carry.** The grid is cells and not a byte
stream (Concept.md §2.3), so there is no escape sequence to put in the bytes and
the colour is an operation instead — two palette indices and the `ATTR_*` bits,
packed into the op word's argument by `sys_style_pack`, so it stages nothing. It
is *sticky* grid state, exactly as it is for the kernel's own writers: whoever
sets a colour puts the default back after it. Refused with `Err(Perm)` while
another process holds the alternate screen, as a cursor set is; a program that
has the alternate screen paints its own cells and names their colours in them.

Like `Cursor`, it has **no caller in the tree**: `Echo` carries the prompt's
colours as runs now, and the reset that corrects a program which died mid-colour
is the last of them. It stays for the same reason — `Echo` is its fused form for
the one caller that pays a round trip per operation, not its replacement. A
program colouring a word on stdout has no anchor to name and does not want a row
of its own, which is the one thing `Echo` cannot express. Neither operation is
speculative: Concept.md §4.3's "every operation has a caller in `src/cmd/`" bars
*growing* the table on a guess, and re-adding either later would cost an ABI
bump that invalidates every stamped binary.

**`Tty` is the question the terminal being a grid makes unaskable.** There is no
escape sequence to send, and a `COLUMNS` in the environment would be a copy
taken at spawn that the first resize made wrong, so a program cannot tell its
own stdout from a pipe. The kernel can: `stdio_console()` installs one sink and
a pipe or a file installs another, and `tty_is_console` (`src/user/tty.h`) is
that difference given a name — with `console_is_input` (`src/user/console.h`)
answering the same for fd 0. `Spawn` bit-copies the parent's `Stream` when a
child shares stdout, so the answer follows a chain of spawns with nothing
carrying it: `ls` under `/bin/sh` under init still says yes. A descriptor from
the process's own table is a file, a pipe, a socket or a pick set, so it says
no; a number naming nothing is `Err(Invalid)`.

It is a get with no state, so none of `Cursor`'s refusals apply — knowing the
shape of your own output is not a claim on the screen. The geometry rides on the
reply for `KeyRead`'s reason, a resize needing no event to subscribe to, and is
**zero when the answer is no**: a pipe has no width, and one invented here would
be believed. The reply is a *flags* word rather than a bare status so a second
fact about a terminal costs no thirty-eighth operation, which is what
`SYS_STORE_*` bought `Storage`. `/bin/ls` is the caller: with the grid it lays
out in columns, and into a pipe it prints one name per line, which is what keeps
`ls | grep` meaning what it did.

**`Fg` decides where `^C` goes.** The console keeps a set of foreground pids;
the pump cancels them all on `^C`, and delivers the interrupt as an ordinary key
to whoever holds the raw route when the set is empty (Concept.md §3.5). Each
call *adds* one pid, because the op word carries one and a pipeline is up to
eight; `Fg(0)` clears the set. The caller must own the terminal — holding the
raw keys, being in front itself, nobody being in front, or having armed what is
in front — and the pid must be one of its own children, exactly as `Kill`
requires. The last clause is what lets a shell arm a pipeline: it lets go of the
keys before it spawns, so from the second stage on it owns none of the other
three. `/bin/sh` is the caller, and the reason the operation exists: without it
a shell that is a process is cancelled by the interrupt meant for the command it
just ran.

**`Spawn`'s three descriptor words.** Slot *i* is either below `SYS_FD_MIN` —
"share the stream I was given" — or a descriptor of the caller's, which is
**moved**: unbound from the parent's table and never bound into the child's,
because 0, 1 and 2 are not table entries on either side.

| Slot | Shares | Moves | Refused |
|---|---|---|---|
| 0 (stdin) | `0` | a `PipeRead` or a `File` | anything else, `Err(Invalid)` |
| 1 (stdout) | `1`, or `2` for the caller's stderr | a `PipeWrite` or a `File` | `0`, `Err(Invalid)` |
| 2 (stderr) | `2`, or `1` for the caller's stdout | a `PipeWrite` or a `File` | `0`, `Err(Invalid)` |

Naming the same descriptor in two slots is `Err(Invalid)` — it would be one
handle owned twice. A `Body`, `Socket`, `PickSet` or `PickFile` is `Err(Perm)`:
those are read by a host round trip rather than through a `Source`, so they are
not stdio whatever they are pointed at. A builtin is not refused here any more,
because the kernel has never heard of one: they live inside `/bin/sh` and
`exec_resolve` looks only in `/bin`, so `spawn("cd")` is an ordinary
`Err(NotFound)`.

**`Spawn`'s environment.** With `SYS_SPAWN_ENV` in the op word's argument, an
env blob follows the argv one and is the child's; without it the child inherits
the caller's, which the kernel holds on the `Proc` record beside the cwd. Both
blobs are the encoding of `src/kernel/sysabi.h`'s argv — the env's words being
`NAME=value` — and the join is found by walking the first (`argv_bytes`), so a
malformed blob is `Err(Invalid)` rather than a child entered with rubbish. An
environment over `SYS_ENV_MAX` is refused the same way. The kernel does not read
the words: a word with no `=` is the caller's business, exactly as an argv word
is.

**Statuses are clamped to 0–255 when recorded.** `Sys::Exit` takes whatever the
program passed it, and a negative status on this wire is an error code.

**The bounds are `SYS_CHILD_MAX` (16 live children) and `SYS_PROC_DEPTH` (16
levels).** Past either, `Spawn` is `Err(NoMemory)` — deliberately not
`Err(Again)`, which `proc_syscall` retries for ever. Every child is an instance
with a 16 MB cap, so without them the first fork bomb takes the page with it. A
shell reports that error as `too many processes` and 126, which is as close as
it can get: one `Error` value carries both bounds and a genuine allocation
failure.

### Constants

| Name | Value | What it is |
|---|---|---|
| `SYS_STDIN`/`STDOUT`/`STDERR` | 0, 1, 2 | the stage's `Stdio`, not the descriptor table |
| `SYS_FD_MIN` | 3 | the first index into the process's own table |
| `SYS_CHUNK` | 512 | one read, and the most a write should hand over at once |
| `SYS_STAGE_MAX` | 1 MiB | the cap on `Sys::Stage`, which is the largest blit there can be |
| `SYS_BLIT_HEAD` | 7 | `ScreenBlit`'s header, in `u32`s |
| `SYS_O_READ`…`APPEND` | 1, 2, 4, 8, 16 | open flags, restated rather than shared with the VFS |
| `SYS_KIND_FILE`/`DIR` | 0, 1 | what `Stat` and `List` report |
| `SYS_STORE_*` | 1, 2, 4, 8 | OPFS, sync, persisted, and "the host answered at all" |
| `SYS_SPAWN_HEAD` | 3 | the descriptor words before `Spawn`'s argv blob, in `u32`s |
| `SYS_SPAWN_ENV` | 1 | `Spawn`'s arg bit: an env blob follows argv, else the child inherits the caller's |
| `SYS_ENV_MAX` | 8192 | the most an environment may be, and therefore what a chain of children carries |
| `SYS_ECHO_HEAD` | 4 | `Echo`'s anchor, cursor offset and run count, in `u32`s |
| `SYS_ECHO_RUN` | 2 | the style and length of one `Echo` run, in `u32`s |
| `SYS_ECHO_RUNS_MAX` | 8 | runs per `Echo`; a prompt is four |
| `SYS_ECHO_SHOW`/`FRESH`/`END` | 1, 2, 4 | show the cursor after, anchor on a fresh row, end where the write did |
| `SYS_STYLE_KEEP` | 0xffffffff | an `Echo` run naming no colour, so the sticky one stands |
| `SYS_TTY_CONSOLE` | 1 | `Tty`'s flags word: this descriptor is the cell grid |
| `SYS_WAIT_ANY` | 0 | `Wait`'s "whichever finishes first"; zero is never a pid |
| `SYS_PID_MAX` | 999999 | the largest pid there is; above it are the scheduler's anonymous jobs |
| `SYS_CHILD_MAX` | 16 | live children per process |
| `SYS_PROC_DEPTH` | 16 | how deep a chain of spawns may go |

`SYS_PID_MAX` is the boundary between the two id spaces, not the op word's limit
— the argument is 24 bits and would carry a larger number whole. Above it the
scheduler names the jobs it runs for itself, one per parked syscall, and `Spawn`
refuses to hand one back. Nothing here can name one anyway: `Wait`, `Kill` and
`Fg` all search the caller's own children, and `/proc` does not list them. Pids
below it are reused once nothing names them any more (Concept.md §4.3).

`SYS_O_*` and `SYS_KIND_*` are deliberately *not* the VFS's numbers. A process
cannot see the filesystem, and the numbers a binary compiled today speaks must
not move because the VFS's did; `vfs_flags` (`src/user/syscall.cpp:33-47`) maps
between them one bit at a time.

`SYS_CHUNK` being 512 rather than a rounder number is the allocator: `FS_BLOCK`
is the top size class on both sides of the wire, and one byte more costs a whole
64 KiB span (§8.2).

### Errors

The wire carries `src/kernel/result.h`'s `Error`, negated: `Invalid` 1,
`NoMemory` 2, `NotFound` 3, `Exists` 4, `NotDir` 5, `IsDir` 6, `Perm` 7, `Io` 8,
`Cancelled` 9, `Again` 10, `Unsupported` 11, `Closed` 12, `NotEmpty` 13.
`web/abi.js:9-13` mirrors the list.

Two never reach a process. `Again` is retried inside `proc_syscall` rather than
reported, and `Cancelled` means the process is being destroyed, so `serve()`
returns without building a reply at all.

---

## 9. Descriptors

Descriptors 0, 1 and 2 are not in the table: they are the `Stdio` the pipeline
stage was given, so a process writing to fd 1 writes into whatever the shell
connected — the screen, a pipe, or a redirection — through the same `Stream` the
console uses. Everything from 3 up indexes a `Vec<Handle *>` on the kernel's
process record.

A `Handle` is a descriptor whatever is behind it, and there are seven kinds:

| Kind | Made by | Read | Write | Closed by |
|---|---|---|---|---|
| `File` | `Open` | `vfs_read` at its own offset | `vfs_write` | `Close` |
| `Body` | `Fetch` | `http_read` | — | `Close` |
| `Socket` | `WsOpen` | `ws_recv` | `ws_send` | `Close` |
| `PickSet` | `Pick` | — | — | `Close` |
| `PickFile` | `PickOpen` | `pick_read` | — | `Close` |
| `PipeRead` | `Pipe` | the channel, EOF when the writer goes | — | `Close`, **or being moved into a child** |
| `PipeWrite` | `Pipe` | — | the channel | `Close`, **or being moved into a child** |

Making the host services descriptors is what lets `Read`, `Write` and `Close`
serve all of them. The alternative was a `fetch` family, a socket family and a
picker family — perhaps six more operations — but saving those is not the reason
to prefer it. The reason is what happens on `^C`: the process's handle table
dies with the process, `~Handle` releases the externref slot, and the socket
closes with no code written for it.

**A `File` descriptor is a reference on a shared VFS handle.** Two `Open`s of
one path — from one process or from two — give two descriptors with two offsets
over one backend handle, and the last `Close` is what reaches the filesystem. An
`Open` is `-Error::Perm` if a writer holds the path, or if it asks for
`SYS_O_WRITE` or `SYS_O_TRUNC` while anyone holds it: OPFS takes an exclusive
lock, and sharing is what keeps that one rule on every mount (Concept.md §5.2).

A `PickFile` remembers its set **by descriptor rather than by pointer**, so
closing the set first is `Err(Invalid)` at the next read rather than a dangling
reference. A set closed *during* a read is a different matter, and is held for
the length of it.

**A descriptor is held for the length of a syscall.** A process has several
tasks, so one of them may `Close` a number another is parked on. `Close` frees
the number at once and *shuts* what the descriptor is on at once — the socket
closes, the body is cancelled, the pipe end hangs up, which is what answers the
parked call, and it answers with the end of a stream rather than an error. What
waits for the last call is the `Handle` block, and the externref slot inside it:
the slot has to stay reserved because `jsref_release` recycles it and a request
already issued names it, so a freed slot could be re-read as somebody else's
object on a second attempt.

**One task uses a descriptor at a time, in each direction.** A second concurrent
read, or a second concurrent write, is `Err(Perm)`. On a pipe end that is
`Channel`'s rule (§9.1); on the host kinds it is that a reply sized twice is not
re-entrant per object and the read offset would advance twice. Reading and
writing one socket at once is fine, and is what `chat` does.

An empty read is the end of a stream, for all of them: a file at EOF, a hung-up
pipe, a finished body, a socket whose peer has gone. `Error::Closed` from the
kernel side becomes status 0 rather than an error, and `read_chunk` turns *that*
back into `Err(Closed)` for the program (`src/proc/io.cpp:17-29`).

### 9.1 A pipe, and why the descriptor moves

A `Sys::Pipe` is one `Channel<String>` on the heap, refcounted, with a `Handle`
on each end. The ends may be closed in either order and may by then be held by
two different processes, so neither owns the channel outright. Dropping the
write end is `close()` — the reader drains what is queued and then reads end of
input; dropping the read end is `hangup()`, which stops the writer.

**Two rules follow from `Channel` rather than from taste**, and both would
otherwise be a user program reaching a kernel invariant:

- **One process holds one end.** A spawn *moves* the descriptor rather than
  duplicating it, so there is no second copy for the parent to forget to close.
  That is what makes the reader's end of input arrive at all: with a copy still
  open in a process that will never write, the channel is never closed and the
  read parks for ever. It also means two blocked senders — which
  `Channel::park_sender` answers with `panic` — cannot be arranged.
- **One task uses one end at a time.** Within a process, a second concurrent
  read or write on the same end is `Err(Perm)`. A second blocked sender panics;
  a second suspended receiver is displaced *silently*, which is worse. Both are
  refused before `Channel` sees them. §9 states the general form of this: it
  holds for every kind of descriptor, for a weaker reason on the host kinds.

An end a syscall of this process is parked on **cannot be moved at all** —
`Err(Perm)` — since a parked reader in the parent plus the child's stdio would
be the second receiver the move exists to rule out. And the move is
all-or-nothing: a spawn that names three slots and is refused on the third takes
none of them, so a refusal leaves the parent's table exactly as it was.

**Drain before you wait.** A child parked on a full pipe has not exited, so a
parent that waits before reading is waiting on a child that is waiting on the
parent. The kernel cannot break that — POSIX has the same deadlock — and only
`^C` will, since every await is cancellable. `watch` is written the right way
round and says so where it does it.

---

## 10. The terminal

The screen is a cell grid in linear memory, not a byte stream (§2.3) — and a
process cannot be handed the kernel's grid, because it is in another address
space. So a full-screen program paints a grid of its own and blits the damaged
rectangle across with one syscall, cursor included:

```
   payload:
   ┌────┬────┬────┬────┬──────┬──────┬────────┬─────────────────────┐
   │ x  │ y  │ w  │ h  │ cur_x│ cur_y│ cur_on │ w*h Cells, row-major│
   └────┴────┴────┴────┴──────┴──────┴────────┴─────────────────────┘
     28 bytes of header (SYS_BLIT_HEAD u32s)
```

`proc_syscall` validates it completely — `x+w <= cols`, `y+h <= rows`, and the
payload length *exactly* `head + w*h*sizeof(Cell)` — then copies row by row into
`screen_cells()` and marks the damage. Anything else is `Err(Invalid)`.

`src/ui/` became a library over a `Grid` for exactly this: `Pane`, `TextBuf` and
`TextView` link into `less` and `edit` unchanged, and the kernel does not link
them at all. Had the terminal been a byte stream this would have been an
escape-sequence dialect instead.

**Both claims are the kernel's, not the program's.** `KeyClaim` and
`ScreenEnter` create a `KeyInput` and a `FullScreen` on the process's
kernel-side record, and `~Proc` destroys them. A killed process runs no
destructor of its own, so a program that had taken the screen and then met `^C`
would otherwise leave the shell painting into a grid it does not own. Giving
them back is politeness; the destructor is the guarantee.

**One holder of each, system-wide, named by pid.** A second `ScreenEnter` or
`KeyClaim` is `Err(Perm)` rather than nesting, whether it comes from the process
that already holds the route or from another one — a parent and its child
included. A claim clears its route only if it is still the holder, so the two
may be destroyed in either order. Nesting would mean restoring a predecessor
that has already gone, and for `ScreenEnter` it would mean snapshotting the
blanked grid the first claimant is painting, which loses the shell's screen
instead of giving it back.

`ScreenBlit` is held to the same rule: from a process without the screen it is
`Err(Perm)`, since otherwise it would paint over whichever process does hold it.
`ScreenClear` is not — `clear` and `watch` blank the shell's own screen without
ever claiming it.

**Geometry rides on every key.** `KeyRead` answers with `code`, `mods`, `cols`
and `rows` together, so a program that repaints per keystroke handles a resize
without an event to subscribe to. `ProcScreen::next_key` reallocates its grid
from those two numbers and marks the whole thing damaged.

`^C` is never routed to a claimant, so a full-screen program stays killable by
the key that kills everything.

---

## 11. The process's own runtime

`src/proc/` is a separate binary's whole libc, and it is deliberately tiny —
every binary carries its own copy, so anything substantial belongs in a syscall
where it lives once in the kernel.

**The scheduler is two arrays.** `Rt` holds `PROC_TASKS` tasks and `PROC_TASKS`
waiters, one each (`src/proc/rt.cpp:26-33`). Task 0 is the root. `proc_spawn`
fills a slot from 1 up and resumes the new task at once, so it runs to its first
suspension before returning — `chat` is the only caller, and one extra task is
all any program has needed.

**The process ends when the root task returns**, whatever the others are doing,
exactly as a process ends when `main` does. The kernel then drops the instance
and cancels the servers of anything the other tasks had outstanding.

**The heap is up before `_start`, because `_alloc` is.** The host places argv in
the process's memory before it can enter the program, so `ready()` runs from
whichever export the host reaches first. It calls `heap_init(0)` and then
`__wasm_call_ctors()`, in that order, so a static constructor may allocate.

**`status_of` is the whole return convention.** 0 means exited, anything else
means suspended. On the way out it issues `Sys::Exit` with the root task's value
— a program that never calls exit still reports one, and a root task whose frame
would not allocate reports 1.

**A reply's data is valid until the next syscall on that slot.** `await_resume`
clears the waiter's `String`, which zeroes its length and keeps its buffer, so
the `Str` handed back stays good until that slot is reused. Every wrapper that
needs the bytes to outlive the call copies them — which is why `read_chunk`
returns a `String` and not a `Str`.

**`panic` is `__builtin_trap()`.** A process has no host import to log through,
so the kernel reports the trap as a crash. It is declared in `host.h` and
defined once per binary, and it takes `(ptr, len)` rather than a `Str` because
the wasm ABI passes an eight-byte struct indirectly — which cost 2,812 bytes
across the kernel's hundred call sites when it was a `Str`.

### What the tests assert of every binary

`test/run.mjs:158-199` is where the ABI is enforced, and it is worth reading as
documentation rather than as a test:

- **Imports are a whitelist checked as a subset**: `env.memory` is required,
  `kernel.sys` and `kernel.sys_async` are permitted, and *anything else fails*.
  A host import in a binary would mean the process ABI had been gone around.
  `sys_async` is not required, because `--gc-sections` removes it from a binary
  that never awaits — `true` is the case.
- **Exports are an exact list**: `_alloc`, `_free`, `_resume`, `_start`. Note
  that `memory` is not exported; it is imported, which is what makes the cap the
  kernel's.
- **Exactly one `braam` section**, with the right magic and `abi`, and
  `max_pages` of 256.
- **The same lists for every binary.** They are identical because there is one
  way to run a program, and the section says nothing about placement for `exec`
  to read.

---

## 12. Writing one

A program is one file in `src/cmd/` defining one function:

```cpp
#include "proc/io.h"

Task<i32> proc_main(Args args)
{
    co_return 0;
}
```

That is `true.wasm`, and because it never awaits, it does not import `sys_async`
at all.

A filter is the shape almost every program has:

```cpp
Task<i32> proc_main(Args args)
{
    Input in(args.tail(), SYS_STDIN, "wc"); // named files, or stdin

    for (;;) {
        Result<String> r = co_await in.read();
        if (r.is_err()) {
            if (r.error() != Error::Closed)
                co_return r.error() == Error::Cancelled ? 130 : 1;
            break;                         // Closed is the end, not a failure
        }
        ...
    }

    Buf<48> b;
    b.put(lines).put(' ').put(words).put('\n');
    if ((co_await write_all(SYS_STDOUT, b.str())).is_err())
        co_return 1;
    co_return 0;
}
```

Four conventions there, repeated across `src/cmd/`: `Input` decides
files-or-stdin in its constructor and opens each named file only when the read
reaches it, reporting one that will not open on stderr itself; `Error::Closed`
is a normal end and `Error::Cancelled` is exit 130; and output is formatted into
a stack `Buf<N>` and written once.

`src/proc/io.h` has a wrapper for every syscall, each a `Task<Result<T>>` that
does one `co_await sys_call(...)` and unpacks the reply. Nothing in a program
should be calling `sys_call` directly except where there is genuinely one
operation and no wrapper to justify — `clear` is the only such case.

### Building it

One line in `src/cmd/CMakeLists.txt`'s `BRAAM_BIN_LIST`, and `braam_add_program`
does the rest: link against `braam_proc`, `--import-memory` with
`--initial-memory` set from `BRAAM_BIN_INITIAL_PAGES`, then `tools/stamp.py`.

That is the in-tree half. The same function is installed by `make install` and
shipped in `braam-sdk-<version>.zip`, so a program can be built outside this
repository and dropped onto a running system without rebuilding it —
[Programming_Manual.md](Programming_Manual.md) is that story.

`--import-memory` with no declared maximum is what makes the 16 MB cap the
kernel's decision. `-Wl,--max-memory=16777216` would also work and would be the
*binary's* number; this way a binary cannot ask for more by being compiled
differently.

A worker of its own is what the recipe gives a program, and nothing in
`src/cmd/` asks for anything else. A program that cannot afford two
`postMessage` hops a syscall may give it up —
[Programming_Manual.md](Programming_Manual.md) §7 has that, and what it costs.

---

## 13. What this does not buy

The honest closing. Each of these is absent on purpose, with the argument
recorded in `Release_Notes.md`; `CLAUDE.md`'s "Known gaps" is the current list.

- **No CPU metering.** A runaway program is killed; nothing bounds one. Fuel
  injection is the only way to bound rather than end, and it is unbuilt.
- **No namespace isolation.** A process has a working directory of its own, but
  no *root* of its own: once a path is absolute, `open` resolves it with the
  kernel's full authority. Fixing that needs a per-process mount view, which is
  a milestone's worth of work in the VFS rather than a line in the dispatcher.
- **An instantiation per command**, roughly a millisecond, plus reading the
  image out of the store. The host caches the compiled `Module` by path, so the
  bytes still cross the VFS on every `exec` and only the compile is saved.
- **Duplication.** With no dynamic linking, every binary embeds its own
  allocator, string types and coroutine runtime. That is why the boot archive is
  ~400 KB where four binaries once cost 47 KB, and why the process-side runtime
  is kept deliberately minimal.
- **Every asynchronous syscall parks.** `await_ready()` is false
  unconditionally; there is no fast path for an answer the kernel already has.
- **One process at a time may hold the screen.** `Pane` is a primitive, not a
  multiplexer.
- **Two fidelity losses from the worker**: a binary that will not instantiate
  reads as a crash, and `Sys::Now` is relative.

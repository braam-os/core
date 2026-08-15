# Release notes

Reasoning, alternatives and trade-offs behind the code. Comments in the source say *what* a
thing is; this file says *why* it is that way. [Concept.md](Concept.md) remains the
specification — where this document and the spec disagree about intent, the spec wins and one
of the two needs amending.

---

## A prompt with a colour

The prompt is bright white, and the `[N]` in front of it — the status of the command that just
failed — is red. That is two lines of taste and one operation in the §4.3 table: `Sys::Style` at
70, which makes the table thirty-five and `PROC_ABI` 5.

**Why it needed an operation at all.** §2.3 says the terminal is a cell grid and not a byte
stream, so there is nowhere in a `write` for a colour to ride: an escape sequence is precisely
what the invariant exists to refuse, and the kernel's `screen_write` would paint `\x1b[1m` as five
cells. The kernel has had `screen_style` since M2 and `show_motd` is its caller — green, then
white again — but a process could not reach it. What a process *could* colour was a grid of its
own, blitted with `ScreenBlit`, and that is refused without the alternate screen; taking the
alternate screen blanks the grid the prompt lives in. This is the same wall `Sys::Cursor` was
built against when the shell became a program, and the answer is the same shape.

The argument fits in the op word — `fg | bg << 8 | attrs << 16`, three bytes of the available
three — so the operation stages nothing and costs one park and one step, the cheapest an
asynchronous syscall gets. It carries no reply data either. The rule that keeps the table honest
is that every operation has a caller, and this one has exactly one: `/bin/sh`.

**Sticky, and the prompt is what cleans up.** The style is grid state, not a property of a write,
which is how it already worked for the kernel. So the convention is that whoever sets a colour
puts the default back, and `anchor` does: red for the status, bright white for the `$`, then plain
white before the cursor comes back — so what is typed is ordinary text and so a program the line
starts inherits nothing. That last reset is also the repair for a program that dies halfway
through a colour of its own. The screen cannot stay wrong for longer than one command, because the
next prompt names all three fields unconditionally.

The alternative — a colour argument on `Write`, or a style word in a write's payload — was worse
in the way that matters: it would put a second meaning into the one operation every program uses,
and a pipe would have to carry it or drop it. `Style` is refused from a process that does not hold
the screen while somebody else does, exactly as a cursor set is, and needs no rule beyond that.

**A prompt now costs three more syscalls**, one per style run, on top of the two `Cursor` calls and
the write it already made. It is once per line rather than once per keystroke, so §4.4's cost model
lands where it lands; the six-tick keystroke is unchanged, since `redraw` sets no style.

The smoke test checks the colours rather than only the text: green motd, bright-white prompt under
it, and after `false` a red `[1]` with a bright-white `$` beside it. The prompt assertion used to
read "not green" — inheriting was the bug it was written for — and now reads "bright white", which
is a stronger statement about the same cell.

---

## Selecting with the mouse

The terminal is a cell grid in shared memory, which means the page can read what is on the screen
without asking anyone. So a mouse selection costs **nothing in the kernel**: no import, no export,
no field in the `Screen` descriptor, no key, no syscall, and no way for a program to find out that
one exists. `web/braam.js` turns a drag into device pixels, `web/render.js` turns those into cells,
and the text crosses back to the page when the drag settles. Concept.md §3.5 says so now.

The alternative was a mouse event alongside `key()`, which several people would call the obvious
design — a terminal that has a mouse usually tells its programs about it. It buys nothing here.
Nothing in userland wants a click: there is no `less` mouse mode, no menu, no button. What was
asked for was **selection**, and selection is a view over the grid, not input. Putting it in the
kernel would have meant a selection in the descriptor, a rule about who may clear it, and a claim
to arbitrate two programs wanting it — which is §3.5's terminal-claims machinery again, for a
highlight the renderer can draw by swapping two colours.

**Swapping two colours is exactly what it does.** The cursor was already drawn and never stored,
by reversing the cell it sits on; a selection reverses the cells it covers, through the same
parameter. The two XOR rather than OR, so the cursor inside a selection is a hole in it — which is
the classic behaviour, and it falls out of the arithmetic rather than being arranged.

### Ctrl+C, twice overloaded

There is no second copy key to give it. `Ctrl+Shift+C` is Chrome's inspector and cannot be taken;
a menu is not something a canvas has. So the chord is overloaded the way Windows Terminal
overloads it: **`Ctrl+C` copies when there is a selection and interrupts when there is not.** For
that to be safe rather than a trap, a copy has to clear the selection — otherwise the second
`^C` in a row would silently fail to reach a runaway program, which is the one thing `^C` must
never do. A click that never left its cell clears too, which gives an obvious escape.

The write goes through `navigator.clipboard.writeText` **in the keydown handler**, not through
`Sys::Clip` and `web/svc.js`. That looks like a duplicate of the clipboard service and is not one:
a service reply reaches the page a turn or more after the keystroke, and by then the transient
activation that permits a clipboard write is gone — the same asymmetry §A.2 records for reading,
where the way out was to wait for a `paste`. There is no equivalent trick for writing, so the text
has to already be on the page when the chord arrives. That is why the worker posts the selection
across when a drag ends rather than when a copy is asked for, and why the page keeps a copy of it.

The `copy` event was the other candidate — it needs no permission at all. It is not reliably
dispatched to a focused canvas with no document selection behind it, and when it is not, the chord
would be swallowed with nothing copied and no interrupt sent. `writeText` inside a gesture is
supported everywhere and fails loudly.

### Select all, which Ctrl+A cannot be

`Ctrl+A` is the line editor's beginning-of-line, and unlike `Ctrl+C` it has **no disambiguator**:
copy could be overloaded because "is there a selection?" answers which of the two was meant, and
there is no such question for select-all. So it is the platform's own chord instead — `Cmd+A`, or
`Ctrl+Shift+A` where there is no `Cmd`. `Cmd+A` was free for the taking: `Key::printable()`
excludes `MOD_META`, so a `Cmd` chord already reached the kernel and did nothing at all. The
Ctrl+Shift form is the weaker half of the pair, since Firefox and Chrome both bind it to browser
UI that a page cannot intercept; where that happens the Mac chord is the only one, which is worth
knowing but not worth a third binding to work around.

Selecting all made a smaller decision visible: **trailing blank rows do not travel with a
selection.** The grid is a fixed rectangle, so select-all on a screen with three lines of output
on it would otherwise hand over three lines and thirteen newlines, and a drag past the last line
of output would do the same on a smaller scale. Blank rows *between* lines are content and stay;
it is only the run at the end that goes.

### What it costs to repaint

A selection changes on every mouse move that crosses a cell boundary, and the highlight is painted
by the same `present()` the kernel's damage rectangle drives — so the renderer repaints the row
span the old and new selections cover between them. Over-painting a few rows during a drag is
cheaper than reasoning about the symmetric difference of two linear spans, and it goes through the
path that already existed rather than a second one.

The selection dies on the next keystroke and on a resize, because the grid has no line model and
no scrollback (§3.5): the cells it names mean something else as soon as anything scrolls. Holding
a selection across output would need the per-row continuation bit that resize re-wrapping has been
waiting for since M7, and it should land with that or not at all.

### The renderer finally has a test

`web/render.js` had none — there is no canvas in Node, and it was the one shipping file with no
coverage at all. The selection is arithmetic over the grid, which is testable with a fake `ctx`
that records what it was asked to fill, so `test/run.mjs` now drives a `Renderer` over **the real
screen** the smoke test has just filled by running `echo hello`, and asserts what the drag reads
back, which cells came out reversed, and that select-all and a drag over everything agree. Both
halves of the first pair earn their place: the first version
checked only the text, and a deliberate break of the highlight logic passed it. Painting and text
now share one `bounds()` so they cannot disagree about what is selected, and mutating it fails the
suite.

---

## A boot that says why there is no prompt

Since the shell became a program, a boot archive that will not give up a runnable `/bin/sh` leaves
a terminal with a version banner and nothing under it. init said one line for it:

```
braam: /bin/sh: not found — the boot archive is broken
```

which was printed for three unrelated causes and was a lie for two of them. `exec_resolve` had the
answer in a `Result` init threw away.

### The error had to be widened before the message could be

`exec_meta` refused a binary with `Err(Invalid)` whether it had no `braam` section, was not a wasm
module at all, or was one of ours built against a different kernel. Concept.md §4.3 has promised
since M8 that the `abi` word makes *"a stale binary a diagnostic rather than a wrong answer"* — but
the wrong answer and the stale binary were the same value, so the diagnostic could not be written
however carefully init phrased it.

So the magic check keeps `Invalid` and the `abi` check becomes `Err(Unsupported)`. That is the
whole mechanism; everything else is rendering. It was tempting instead to have `exec_meta` hand
back the number it found so the message could name both, but the value it returns is the metadata
and there is no metadata to return — a section with a foreign `abi` is not a `ProcMeta` this kernel
can read. Naming the kernel's own number is enough to place the fault: a reader who sees "this
kernel speaks 4" knows to look at what built the archive.

The shell gets the same distinction for free, since `Sys::Spawn` resolves through the same
function. A stale archive breaks every typed command, not just the boot, and `braam: echo: built
for another process ABI` is a better first thing to see than `not executable`. The exit code stays
126: found, and would not run.

One trap on the way. The natural way to write that is

```cpp
Str why = e == Error::NotFound ? "not found" : e == Error::Unsupported ? "..." : "...";
```

and it does not link. `Str`'s length comes from `__builtin_strlen`, which folds only when the
pointer is a constant; a two-way conditional clang still sees through, a three-way one it does not,
and the result is a call to `strlen` with no libc to satisfy it. A literal per branch, and the fold
comes back. This is `--allow-undefined` being absent doing its job (§C.3): the alternative was a
runtime trap in the diagnostic path, which is the one path nobody exercises.

### The greeting

`bundle/share/motd` had shipped in the archive since there was an archive and nothing had ever
printed it — only `run.mjs` read it, as a convenient read-only file. init prints it now, in green,
restoring the default style afterwards so the prompt beneath it is not green too. It is the
kernel's first use of colour; `screen_style` had no caller at all.

It is printed **after** the shell resolves rather than after the mounts, which is the whole of the
ordering decision: a system that is not going to reach a prompt should show why, not a welcome
above a dead terminal. The two are never on screen together.

Absent is not an error and says nothing. A boot archive without a greeting is not a broken one, and
a line reporting its absence would be noise at every boot of one.

`show_motd` is a coroutine of its own rather than four lines inside `init_task`, for the reason
`boot_filesystem` is one: it holds the whole file, and a frame past 512 bytes costs a whole 64 KiB
span (§8.2). That is also why `read_file` moved out of `exec.cpp`'s anonymous namespace into
`src/user/io.{h,cpp}` — `boot.cpp` could not reach it where it was, `io.h` already holds `FileIo`
and `file_open_read`, and the process-side twin is declared in exactly that place in
`src/proc/io.h`.

### The farewell carries the status

`braam: the shell exited (status 7) — reload to start again`. A shell the user typed `exit` at and
one that died on its first step looked identical from init, and the second is the interesting case:
`exec_process` prints `will not instantiate` or `crashed` through `io.err` — which *is* the grid
here — and the status line under it now says which.

### The banner fits on one line again

It was 82 cells and the grid at boot is 80 columns, so it wrapped — and the comment above
`screen_resize(80, 24)` claimed the host's first `resize()` would reflow it, which is not what
resize does: it moves rows and drops from the top, and never re-wraps a logical line (§3.5's
unkept promise to M7). A banner that wraps at boot therefore stays wrapped in a terminal of any
width. Dropping the word `reserved` takes it to 73, with enough headroom for a four-digit revision
count and a slower machine's boot time, and the comment now says what the constraint is so the
next line added to it is measured rather than assumed.

### What is not covered

The three broken-archive paths have no smoke case. Testing them means booting a fresh instance
against a patched `bundle.bin`, which `run.mjs` has the machinery for — `store.reset()` and a new
instance, as the no-OPFS case does — but a corruption is written by hand against the BBND layout
and would be a second, weaker copy of what `test_sysabi` already asserts about `exec_meta`. What
the smoke test does check is the greeting: that it reaches the grid, that it is green, and that the
prompt below it is not.

---

## The shell is a program

`/bin/sh` is a binary in `/bin` that init runs. `src/user/shell.cpp`, `edit.cpp`, `job.cpp` and
the six builtins are gone from the kernel and live in `src/sh/`, compiled into one 81 KB module.
`kernel.wasm` fell from 185,205 bytes to 136,699 — a quarter of it was the shell. The §4.3 table
grew by two, `cursor` and `fg`, and `PROC_ABI` is 4.

There is now **no in-kernel program of any kind**. §4's tier table had one row that was not a
tier — *"none: it **is** the shell"* — and that row is what this change deletes.

**What made it possible was already there.** An earlier scoping of this put it at M8-sized: a
whole syscall family, descriptor passing, per-process cwd, three pieces of kernel state §3.6
refuses. Every one of those had landed in the three changes above by the time this started, and
`watch` was already building a pipeline, moving a descriptor into a child, draining it and reaping
it — in 125 lines. What was left was the terminal, and only the terminal.

### The four things a prompt could not do

1. **Find the cursor.** The line editor read `screen().cursor_x/_y` and called `screen_move` on
   the *scrolling* grid. `ScreenBlit` is the only cursor-setter in the ABI and it is refused
   without the alternate screen — and taking the alternate screen blanks the grid the prompt lives
   in. So: `Sys::Cursor`, get and set in one operation, on the primary screen.
2. **Be given keys at all when nothing is running.** The pump was spawned per foreground pipeline
   and belonged to the job it watched; at the prompt the shell received on `keys()` itself. A
   process has no `keys()`. So the pump became permanent, and init spawns it.
3. **Survive its own `^C`.** The pump cancelled the running pipeline and never routed `^C` to a
   claimant. A shell process holding the keyboard would have been cancelled by the interrupt meant
   to abandon a line.
4. **Name what `^C` should reach instead.** So: `Sys::Fg`.

The last two are one rule: **`^C` cancels the foreground if there is one, and is delivered to the
raw-key claimant if there is not.** A shell at a prompt has nothing in front, so it gets the key;
a shell running a command has put that command in front, so the command gets cancelled. Both
halves fall out of one branch in the pump.

### Handing over the terminal is a race, and the order is the fix

The first version of `run_line` spawned the stages, put them in front, and *then* let go of the
keyboard. `less` under it printed `less: no keyboard`.

A child is an ordinary scheduler job, so it starts running the moment the shell next parks — and
the shell parks on the very next syscall. A full-screen program claims the keys in its first step.
So by the time the shell got round to releasing them, the child had already asked and been
refused. Ordering cannot avoid it: every step between spawn and release is a park.

The keyboard therefore goes back **before** anything is spawned. That costs one thing and it is
worth naming: `Sys::Fg` cannot then require the caller to hold the keys, because the caller has
just given them up. The rule is "the caller must own the terminal — it holds the raw keys, *or* it
is itself in front, *or* nobody is in front", and the last clause is what a shell uses. A
background program cannot use it, because by then its shell's foreground is armed.

The window between the release and the first `Sys::Fg` — a few steps — is the one moment a `^C` is
dropped rather than delivered. Making it airtight needs the spawn and the arming to be one
operation, which would be a worse ABI for a case a person cannot hit.

### The pump cannot outrun a claimant that is a process

`Channel<Key>` holds 64 and a claimant's ring holds 32, and the pump moves keys from one to the
other without ever suspending — `co_await recv()` does not suspend while the channel is
non-empty. A burst of typing arrives in one tick, so a line longer than 32 characters lost its
tail before the claimant was ever scheduled. The kernel-side editor had never shown this, because
it read the 64-slot channel directly.

Dropping is the right policy for a claimant that has stopped reading — it is what keeps a wedged
program from taking `^C` down with it — but it is the wrong answer for one that simply has not run
yet. So the pump waits for room, through the timer queue at zero delay, and drops only after
`KEY_WAIT` of those. The host answers a delay of 0 with another pump straight away, so a claimant
that is reading costs microseconds; one that is not costs 64 near-instant ticks and then a dropped
key, with `^C` still arriving on time.

64 rather than 2 because the claimant is a *process* now: every key it reads is a syscall and a
step, so draining three of them is a dozen ticks.

### Type-ahead survives a claim being dropped

`^C` at a prompt abandons the line, drops the editor's claim, and takes a new one for the next
prompt. Anything typed after the `^C` was sitting in the ring that just went — and used to be
sitting in `keys()`, where the next `read_line` would find it. `~KeyInput` therefore puts what it
never read back on the channel. The pump has drained the channel by the time anything can release
a claim, so this arrives ahead of whatever the host queues next rather than behind it.

### The console is a device, so its end of input is not the channel's

`^D` closes the cooked channel, and the next command has to be able to read. A pipe is closed once
and dies; the console is not a pipe. `Channel::reopen()` undoes `close()` and leaves queued values
and parked tokens alone, which matters: the alternative was rebuilding the channel in place, and a
reader still unwinding from the previous foreground would have been parked on a corpse.

Arming a new foreground is what reopens it, and also what *clears* it: type-ahead meant for what
has gone is not input for what replaces it.

### What the shell gave up, and what it gained

- **`kill <pid>` is gone**; `kill %n` is not. `Sys::Kill` refuses anything that is not a child of
  the caller, and a bare pid the shell never started is exactly that. The authority it needed was
  never the shell's to have once the shell became a process.
- **`/proc/jobs` is gone.** The table is a process's own memory now, and no syscall shows one
  process another's. The stages are still scheduler tasks, so `/proc/<pid>` still lists them —
  which is how the shell notices a background job has finished, since a `wait` would park and the
  prompt has to come back either way.
- **A builtin in a pipeline runs in its turn, not alongside.** Nothing inside a process can wait
  for a sibling task: the only resumption a task has is a syscall reply. So builtins are run to
  completion in pipeline order, and each must write its output *once* — a pipe holds eight chunks,
  and a builtin writing a line at a time would fill one and park with nobody left to drain it.
  `jobs | help` still works, and that is the constraint that keeps it working.
- **`sh -s`** reads stdin a line at a time instead of drawing a prompt, which is what a script is,
  and it costs four lines because the loop was already there.
- **A second shell is now writable at all** — and was, in fact, how this was built. `/bin/sh` ran
  as a child of the resident one for the whole of its development, exercised from the prompt and
  in `run.mjs`, and the flip was the last commit rather than the first.

### The cost, measured

A keystroke at the prompt was one channel receive and a few writes into an array. It is now
`key_read`, then a repaint of four syscalls — cursor off, one write of the whole line, a cursor
read to find the scroll, and the cursor back on — each of them a park, a step and a resume. Six or
so ticks per key.

`run.mjs` had asserted exactly one `present` per keystroke; it now asserts one per *tick*, which is
what M2 actually asked for and what still happens. The rest of the driver needed one other change:
`net.proc.live()` is never zero any more, because the shell is an instance and a permanent one, so
sixteen assertions went through a helper that subtracts it.

### Smaller decisions

- **`Args` was defined twice, identically**, in `src/user/prog.h` and `src/proc/rt.h`. The grammar
  needed it on both sides of the boundary, so it moved to `src/kernel/args.h` and the copies went.
  `parse.cpp` and `tokenize.cpp` now compile unchanged into the shell binary and into the test
  suite, from one source, because they touch nothing but `Str`, `String` and `Vec`.
- **`Sys::Open` did not honour `SYS_O_APPEND`.** Nothing below the VFS is seekable, so appending is
  a starting offset the descriptor's owner keeps — and the kernel-side shell did that for itself
  while `Sys::Open` did not. Latent until a *process* opened a file to append to, which is the
  first thing `sh` does with `>>`.
- **`proc.shutdown()` cleared every process record**, which was correct while none was permanent
  and wrong the moment the shell became one: the test that simulates a host with no workers was
  killing the shell. It split into `shutdown` (dispose: everything) and `dropWorkers` (the pool and
  the tier it backs, leaving tier 2 alone), which is what §4's fallback actually means.
- **`SYS_CHILD_MAX` is 16**, from 8: a pipeline is up to eight stages and a background job the
  shell has not reaped yet still holds an entry. **`PROC_TASKS` is 8**, from 4, because the shell
  is the first process that wants more than two.
- **The shell stays at tier 2.** Tier 3 would put two `postMessage` hops under every keystroke, and
  the kill switch it buys is no use to the one process nobody can be spawned to kill.
- **`bundle.bin` is ~491 KB**, from ~406 KB: `sh.wasm` is 81 KB of grammar, line editor, job
  runtime and builtins, and it carries its own copy of the allocator and the coroutine runtime like
  every other binary. §4.4's duplication, arriving where it always does.

### What this did not do

`/bin/sh` has no variables, no `-c`, no globbing and no scripts beyond `sh -s`. None of that was
blocked by the shell being kernel code and none of it is blocked now; they were simply never
written, and the point of this change was to move the shell without changing what it does.

---

## A descriptor is held for the length of a syscall

The gap the previous change left open — a `Body` or a `Socket` closed by one task while another
is parked reading it — is closed, and closed for every kind at once rather than for the two the
note named. The rule is the one the pipe ends already had, moved up a level:

> A `Handle` is born with one reference, held by the descriptor table. A syscall performed on it
> takes one more for the length of the call. `Close` frees the number and shuts what is behind the
> descriptor **at once**; the block, and the externref slot in it, goes when the last reference
> does.

Nothing about the wire moved: no wasm ABI, no §4.3 operation, no `PROC_ABI`, and no JavaScript.

**It was worse than a stale write.** `h->off += r.value().size()` through a freed block was the
visible half, and `proc_bind` reuses a freed slot before it grows the table, so a `Close`+`Open`
pair under a parked read could land that write on a live descriptor's offset. The half that
decided the shape of the fix is one level down. `~Handle` reaches `~JsHandle`, which tells the
host to let go, and *then* `~JsRef`, which nulls the table entry and pushes the slot onto a LIFO
free list — so the next `ws_open` or `fetch` anywhere in the system takes that slot straight back.
Meanwhile `HostCall::issue()` re-reads `jsref_get` on **every** attempt, and `svc_blob` issues
twice whenever the reply does not fit in 512 bytes, which is the path `ws_recv` and `pick_name`
take for anything long. The window between the first attempt suspending and the second issuing is
exactly where another task's `Close` runs, so a parked socket read could be re-issued against
another process's socket. That is why deferring the free was not enough on its own and the slot
had to stay reserved.

**Shutting is therefore separate from freeing**, and that separation is load bearing in the other
direction too. `Close` on a pipe end must still `close()`/`hangup()` at once, because that is what
wakes the other end; deferring it to the last reference would have left a reader parked for ever
on a pipe whose writer had just gone. So `Handle::shut()` does what the destructor used to do —
`FileIo::reset`, `JsHandle::drop`, `PipeEnd::shut` — and the destructor now only releases. A
parked call sees the end it should: a dropped socket answers `Err(Closed)`, a cancelled body an
empty chunk, a hung-up pipe end of input, all of which were already status 0 on this wire.
`JsHandle::drop()` is the new seam, and it is idempotent because the destructor still calls it.

**`PickFile` was missing from the recorded gap** and had the hazard twice: it shares the `off`
write, and it reads through a *second* descriptor. Resolving the set by number rather than by
pointer was already deliberate — closing the set first should be `Err(Invalid)` at the next read —
so the lookup stays where it was and only the reference is new. A set closed *before* a read is
still refused; a set closed *during* one is held to the end of it.

**The busy guard came along, and its reason is not the pipes' reason.** `busy_r`/`busy_w` moved
off `ProcPipe` onto `Handle`, so one reader and one writer at a time is now the rule for every
kind. On a pipe end that is a kernel invariant: `Channel` panics on a second blocked sender and
displaces a second suspended receiver silently. On the host kinds nothing panics — two outstanding
calls against one slot are well formed — and the honest statement is weaker: `svc_blob`'s
sized-twice reply is not re-entrant per object, so a second reader consumes the message the first
sized itself for and the first exhausts its two attempts into `Err(Io)`; and `off` would advance
twice. A defined refusal beats a race a program cannot avoid other than by not running it. The
guard is per *direction* and had to be: a `Socket` is one `Handle` serving both, and `chat`'s
receiver reads it while the root task writes it, so a `refs > 1` test would have broken `chat` on
the first message typed.

**Two refusals were added to `Sys::Spawn`.** A descriptor a syscall of the parent is inside of
cannot be moved into a child: refcounting makes that case memory-safe without making it correct,
since a parked reader in the parent plus the child's stdio is the second receiver the move exists
to make unrepresentable. And the take is now all-or-nothing.

**The take, and why the commit moved to the end.** The old loop nulled the parent's slots as it
went, so a refusal on the second or third left the first already moved into a `Spawned` about to
be destroyed — the parent silently lost a descriptor on a spawn that failed. It was reachable with
two verdicts already and the new one made three. The fix is a validate pass where the loop was and
a commit pass at the very end of the function, past `sched_spawn` and past the child's entry being
recorded, so every failure leaves the parent's table exactly as it found it. That works because
`sched_spawn` only *queues* the task — a `Task` is lazy, which the surrounding code already relied
on when it assigned `s->pid` after the spawn — and because there is no await anywhere from the
validate pass to the end, so nothing can resume the child in between. A restore guard was the
obvious alternative and is worse: it would hand a descriptor back to the parent while a cancelled
child job still named it through its stdio, which is the two-owners case this change is otherwise
closing. `p.children.reserve` before the spawn is what removes the one failure that would
otherwise have sat between the two passes.

The duplicate-descriptor check at the top of the syscall is now load bearing in a way it was not:
before, two slots naming one descriptor were refused as a side effect of the nulling. After the
split they would validate twice and be committed into two `moved[]` entries, which `~Spawned`
would release twice, so the up-front check is the only thing refusing them and says so.

**What the tests pin.** The unit case is the one that fails without the fix: a socket the host has
been told to let go of still answers a read, and its slot is still counted until the handle goes.
The `chat` case is a characterisation — end of input now closes the connection in the program
rather than leaving it to teardown, which is a better program and also the one sequence a shipped
binary can arrange, since the receiver is parked on that socket while the root task waits for the
`Close` reply. Neither of the two new `Sys::Spawn` refusals is reachable from `/bin`: no shipped
program names two descriptors in a spawn, and none both spawns and reads from a second task. They
are pinned by the ABI and by this note, not by the suite.

**The cost.** Two bools and a `u32` on `Handle`, a `bool` on `JsHandle`, a seven-way `shut()` and
one free function, against the deletion of `PipeRef`, `PipeBusy` and two `ProcPipe` fields —
1,524 bytes on the kernel, which is 70% of its budget rather than 69%, and 189 on `chat` for the
close it now makes itself.

---

## One claimant, named by pid

The terminal's three routes through the tty pump — raw keys, the screen, cooked bytes — now have
one holder each, on the kernel. A second claim is `Err(Perm)` rather than a nested one, a claim
clears its route only if it is still the holder, and the two a process makes carry its pid. This
is the change the previous section left undone, and it changed no ABI: the same five terminal
operations, the same wire format, and nothing in `src/cmd/`, because `less` and `edit` already
treat a refused claim as fatal and say `less: no keyboard`.

**Nesting was a stack with no owner.** `KeyInput` saved the predecessor's ring and `InputClaim`
the predecessor's pipe, so both were correct exactly as long as claims were destroyed in the order
they were made. Nothing enforced that and nothing could: a claim's lifetime is its process's
kernel-side record, and two processes die in whichever order they die. Out of order, `~KeyInput`
restored a ring its owner had already freed and the pump `try_send`s keystrokes into it;
`~InputClaim` had no guard at all and restored a pipe belonging to a job that had finished. Both
are use-after-free, reachable from the prompt with `fg %1 &` twice.

`FullScreen` was worse for being subtler. It saved no predecessor because it saves the *cells*, so
a second claimant snapshotted the blank grid the first had just cleared and handed *that* back on
the way out. The shell's screen was not corrupted so much as quietly replaced by nothing — and
`ScreenEnter`'s `Err(Perm)` did not catch it, because it tested the claiming process's own record
rather than the route.

**Refusal rather than a queue or a stack.** The alternative was to make the claim a real stack
with owners, so a child could take the screen and give it back to its parent. That is a window
manager's problem with a window manager's answer, and `Pane` is a primitive rather than a
multiplexer for the same reason (Concept.md §3.5). Refusal is the answer that costs one branch and
cannot be got wrong: whoever asks second is told no, and finds out by the same `Err(Perm)` a
program already handles.

**The pid is on the two a process makes, and not on the third.** `KeyInput` and `FullScreen` are
built by the `Sys::KeyClaim`/`Sys::ScreenEnter` arm of `exec.cpp`, which has the pid in hand on
`Proc`. `InputClaim`'s only claimant is `fg`, a builtin — a pipeline stage rather than a process,
with no `Proc` and no pid it can name itself with, since the scheduler's ready queue holds bare
coroutine handles and there is no `sched_current()` to add cheaply. So the cooked route is owned by
the claim object instead: same rule, same refusal, an identity that is a pointer rather than a
number. A refused `fg` disowns rather than kills the job it was about to adopt, because the job was
running before it asked and being told no is not a reason to end it.

**`ScreenBlit` is checked; `ScreenClear` is not.** A blit is what the claim exists for, so a blit
from a process without the screen is `Err(Perm)` — it would be painting over whichever process
does hold it. `ScreenClear` stays open because `clear` and `watch` blank the shell's own screen
without ever claiming it, and refusing them would be a change to two programs to enforce a rule
about a third. Ordinary output is the same story a level down: a background job still writes to
the grid through `stdout`, and gating *that* is output routing, not a claim.

**The release stays in `~Proc`.** Moving it into `exec_process`'s `End`, where a process
demonstrably dies, would give the screen back promptly — and would free a `KeyRing` with a
cancelled `KeyRead` server still parked on it, since `End` queues the cancellation rather than
unwinding it. So the release still lags a process's death by the tick or two the last server takes
to unwind, exactly as it did before. What changed is what that window costs: a claim arriving
inside it is refused, where before it was granted and corrupted the grid.

Two tests, because the two halves are testable in different places. `test/unit/test_tty.cpp`
drives the claim types directly — the in-wasm tests cannot run a program, and it builds the
out-of-order case on the heap because a scope cannot express it. `test/run.mjs` covers the
syscall path with `less /share/doc/README | less`, two pagers in one pipeline, which is how a
second claimant is reached from the prompt without writing a program for it. It asserts that one
of them took the screen, that the other said `less: no keyboard`, and that the shell's screen
came back — not *which* stage won, since that is scheduler order rather than contract. Before the
change the second claimant snapshotted the blank grid and the prompt came back to nothing.

---

## Processes that spawn processes

Five operations — `chdir`, `pipe`, `spawn`, `wait`, `kill` — take the §4.3 table from
twenty-seven to thirty-two, and every process gets a working directory of its own. `PROC_ABI` is
3. The shell stays resident: it is not a process, and `cd` is still a builtin.

**What forced it.** A program whose job is to run another program had nowhere to live. It could
not be a builtin — the six that exist are the six no syscall could serve, and running a child
inside the shell's own frame would be the applet tier coming back through the side door — and it
could not be a binary, because a binary had no way to start one. So `timeout`, `watch` and
`xargs` were not unwritten but unwritable, and the gap was in the ABI rather than in `src/cmd/`.
The two that landed are the two the rule allows: every operation has a caller, and `timeout`
covers `spawn`/`wait`/`kill` while `watch` is the one program that wants what its child *printed*
rather than wanting it printed.

**Why `chdir` came with them.** M5 recorded that a per-process cwd had nowhere to live "until
§4.3's ABI gives a process a context, which is M8". M8 landed and the global stayed, which was
defensible while nothing could spawn: with one shell running, a global and a per-process cwd are
indistinguishable. A child changes that immediately — `ls` in a spawned process would walk the
shell's directory rather than its parent's, and `Spawn` would be incoherent before it was useful.
The two arrived together for that reason and not for tidiness.

`chdir` is numbered 24, with the filesystem block, rather than 84 with the process family. It is
the state `open`, `stat`, `list`, `mkdir` and `remove` resolve *against*, so it belongs with the
operations it governs; a program that never spawns anything still moves it. Its caller is `pwd`,
which stopped reading `/proc/cwd` — ProcFs generates a file at `open` and has no way to know who
is reading, so that file can only ever be one answer, and the one answer is the shell's. This is
the one place the "publish it under /proc instead" argument runs out, and it is worth naming: the
question `pwd` asks is not "what is the cwd" but "what is *mine*".

**The dispatcher resolves, and the VFS did not have to change.** The obvious move was a
cwd-parametric twin of each of the six `vfs_*` entry points. None was needed. `path_resolve`
already takes its cwd as an argument and already ignores it for an absolute path, so the five
sites in `proc_syscall` resolve against `p.cwd` and hand the VFS something absolute, and
`vfs_abs`'s second pass is a copy that changes nothing. One helper, five call sites, and
`vfs_cwd`/`vfs_chdir` left alone as what they always were: the shell's. `test_path` now asserts
the idempotence that whole simplification rests on.

**A descriptor moves into a child rather than being duplicated.** POSIX dups and expects the
parent to close its copy; forgetting is the classic bug where the reader never sees end of input,
because a write end is still open in a process that will never write. Moving makes it
unrepresentable, and that is the smaller half of the argument. The larger half is that a
`Channel` has one receiver and *panics* on a second blocked sender — a tripwire M4 put there
deliberately — so two processes holding one pipe end would be a user program reaching a kernel
invariant. One end, one owner, by construction. Within a process the same rule is enforced by
hand: a second concurrent read or write on an end is `Err(Perm)`, because a second sender panics
and a second receiver is displaced *silently*, which is worse.

### The ownership chain, and the bug it was hiding

The subtle half of this change is not the wire. A child inheriting fds 0/1/2 gets a copy of its
parent's `Stdio` — three function pointers and a ctx — and that ctx is a `Pipe` owned by the
shell's refcounted `Job` block, released when the stage's frame is destroyed. A child is an
independent scheduler job and cancellation is deferred, so it would still be unwinding a tick
after the block was freed, deregistering a waiter from a `Channel` that no longer existed.

Writing that down made it obvious that **the bug was already there**, with no children involved.
`~End` cancels a process's syscall servers, but `sched_cancel` only pushes a frame onto the ready
queue, and `sched_tick`'s drain has finished by the time the sweep destroys the stage. A server
parked in `p.io.out.write()` therefore unwinds *after* `StageEnd` released the `Job`. The second
one was smaller and in the same function: `serve` read `c->token` before the `Cancelled` check
that would have told it `~Proc` had already freed the `Call`.

The fix is one rule — *anything that can still touch a `Stdio` it did not create holds a
reference for as long as that is true* — and three holders of it. `Stdio` carries a `hold`/`owner`
pair naming the block behind its three streams (null is the console, which nobody owns). `Proc`
retains it and releases it last in `~Proc`, after the handle table, because a redirected `File`
handle closes through the VFS and lives in that same block. And every syscall server takes a
counted `ProcRef` **by value as a coroutine parameter**.

That last detail is the whole thing, so it is worth stating plainly: a coroutine's parameter
copies are destroyed when the frame is, which is *after* the body's locals — and one of those
locals is the awaitable that deregisters from the pipe in its destructor. The reference therefore
outlives the deregistration by construction rather than by luck. `~End` stops deleting the
record; it marks it dead, unlinks it so a late syscall is `NotFound`, and drops one reference.

A parent-created pipe needs none of this. A `ProcPipe` is refcounted and reachable only through
the handles holding its ends, so moving one into a child transfers the pointer and the child
keeps the channel alive with no `Job` involved. The owner reference is for fds 0/1/2 alone.

**The cost, stated.** Two words on `Stdio`, one `u32` on `Proc`, one refcount bump per syscall,
and a shell `Job` that may outlive `run_line` for as long as a spawned descendant is alive —
bounded, because a process's destructor cancels its children the way `run_line` cancels its
stages. §3.6's structured concurrency, put back by hand a second time.

### What this did not fix

Two things were found while doing it and deliberately not folded in, because each is its own
change and neither is made worse by this one.

The terminal claim assumed it would be destroyed in the order it was made, and `ScreenEnter`'s
`Err(Perm)` was per-process rather than global — a parent and a child made two claimants natural
where they had only been reachable. That is the section above, done since.

And a `Body` or a `Socket` can still have its descriptor closed by one task while another is
parked reading it. The pipe ends take a counted reference for the length of the call; the older
kinds do not.

### Smaller decisions

- **All five are asynchronous**, which costs a park and a step even for a `wait` on a child that
  has already exited. The synchronous half is closed at four permanently and for a reason that has
  not weakened: tier 3 answers those four inside the process's own worker, and a fifth would fail
  at tier 3 alone. The upside is that **no JavaScript changed at all** — `sys_async` is opaque to
  `web/proc.js`, `web/procworker.js` and the fakes, so a new operation really is a number on each
  side.
- **The caps are eight children and eight levels.** Every child is an instance with a 16 MB cap,
  so nothing else would stop the first fork bomb. Past either is `Err(NoMemory)` and deliberately
  *not* `Err(Again)`, which `proc_syscall` retries for ever.
- **`SYS_PID_MAX` is 0xffffff**, because `wait` and `kill` carry the pid in the op word's 24-bit
  argument. `Spawn` refuses to hand back a pid above it: a truncated pid would name somebody
  else's process rather than nothing. Pids are never reused, so the parent a finishing child
  looks up by number cannot be a different one.
- **A status is clamped to 0–255 when recorded.** `Sys::Exit` takes whatever the program passed,
  and a negative status on this wire is an error code.
- **`Wait` holds no pointer across its await.** A concurrent `Spawn` in another task of the same
  process pushes onto the child `Vec` and moves every element, so the parked call re-finds its
  child by pid on the way out as well as on the way in.
- **`Spawn` resolves before it takes the descriptors.** A name that turns out not to be a command
  leaves the parent's table as it found it, and there is no await between taking them and
  spawning, which is what makes the take atomic against another task closing one.
- **`tools/stamp.py` reads `PROC_ABI` out of `sysabi.h`** rather than restating it. The old copy
  fell behind the moment the number moved, and it stamps the field whose whole job is to make a
  stale binary a diagnostic — a wrong copy there is the one place the mechanism cannot report
  itself.
- **The test driver now loops while `tick` reports 0.** It stopped when the *host* had nothing
  left to do, but a wake issued during the scheduler's end-of-tick sweep — a child reporting its
  status to its parent, exactly — lands on the ready queue after the drain that would have run
  it. `web/worker.js` has always answered a delay of 0 with another pump; the driver now does the
  same, which is one fewer way for it to disagree with the browser.
- **The boot archive is ~400 KB**, from ~379 KB: two more binaries, each carrying its own copy of
  everything. §4.4's duplication, still arriving on schedule.

---

## System_Calls.md, and what writing it found

The kernel↔process mechanism was documented in three places that each held a piece of it:
Concept.md §4.3 fixed the ABI and said why it has its shape, this file argued each decision under
M8 and M9, and the source comments said what each line does. Nobody had written it down end to
end. [System_Calls.md](System_Calls.md) does that — the deferred step, the staging protocol,
descriptors, the terminal calls, cancellation and the kill, with sequence diagrams of the calls
that actually happen and the whole twenty-seven-operation table in one place.

**Why a fourth document rather than a longer §4.3.** Concept.md is a specification and is read
by someone deciding whether a change is allowed; it earns its brevity by not explaining twice.
The thing a newcomer needs is the opposite — the same mechanism at length, in the order it
happens, with the code beside it. Those are different documents for the same reason a standard
and a textbook are, and merging them would have made §4.3 worse at the job it already does. So
System_Calls.md is explicitly derived: read it to understand the mechanism, amend §4.3 to change
it.

Two things it says that no file said before, both of which cost time to work out from the code.
**There are two token namespaces**, and they travel together: the host-request token that
`wake()` answers and names a suspended *kernel* task, and the process syscall token that rides in
the step request's `flags` and names which of the *process's* parked awaits a `_resume` answers.
And **`await_ready()` is unconditionally false**, so an asynchronous syscall always costs a park
and a step even where the kernel could have answered at once — which is the cost model a program
author needs before writing a loop, and it was previously only visible by noticing what
`SysCall` does not override.

**The consistency pass.** Reading the whole mechanism against the tree turned up documentation
that had drifted, and it is worth recording what kind, because it was not the kind expected.
Little of it was the applet retirement — that change carried its own notes, and Milestones.md
already has a preamble telling a reader how to read every milestone below it. The stale things
were older and quieter:

- **Concept.md §3.4 listed a `host_random` import that was never built.** Six imports is a number
  asserted by `test/run.mjs` and quoted in three documents, so a seventh name in the block reads
  as an ABI rather than as an intention. It is now marked unbuilt rather than deleted, which is
  the treatment the section already gave `host_fetch`.
- **§3.6 described a `Process` type that never existed.** What the kernel has is a scheduler job:
  a `Task<i32>`, a name and a `CancelToken`. argv and stdio belong to the pipeline stage and the
  working directory is a global, which §4 says two hundred lines further down.
- **§3.6 still promised structured concurrency** — "a parent `co_await`s a child group, and
  cancellation propagates down the tree". M4 found that `CancelState::waiting` is a single slot
  and rebuilt the relationship by hand from a destructor in `run_line`'s frame; the departure was
  written up here and in CLAUDE.md, and the specification was never amended to match. It is now,
  and it names the intrusive queue links a real child-group awaitable would need first.
- **§4.3 said the operation table grew by seven.** It grew by nineteen, from eight to
  twenty-seven: the seven named, plus the host services that hand back a descriptor and the five
  terminal operations. The undercount had survived because the sentence was true when written.
- **§5.1 listed a `/var` nothing mounts and a `HostFs` on `/mnt/host`** that has no
  implementation — and whose name was taken in the meantime by `src/fs/hostfs.h`, which is the
  storage ABI. §5.4's picker-backed `mount` is likewise unbuilt: `vfs_mount` is called from boot
  and from nowhere else, and `mount` the command reformats `/proc/mounts`.
- **§7 said `braam_ui` was a sibling of `braam_fs` and `braam_svc`, above the kernel.** It is in
  neither hierarchy: `braam_proc` links it and `kernel.wasm` does not link it at all, which is
  what "the programs that paint are binaries" means at the build level.

The general shape is that a *name* outliving its referent is caught by grep, and the drift that
matters is not. `Process`, `host_random` and the child-group promise were all found by reading
the document against the code, and a sampling search would have found none of them. Estimates
drift the same way: "roughly 300 lines of JavaScript" for a 176-line renderer, "about 200 lines"
for a 372-line allocator, "a ~25-line shim" for a file that is 124. Those are now the tree's
numbers, or gone where a number was never the point.

Exact byte counts came out of README.md and CLAUDE.md for a related reason. The version header
embeds the commit count and the hash, so `kernel.wasm` moves a few hundred bytes on every commit
and a pinned figure in a living document is stale by the next one. They now say "about 169 KB
against a 256 KiB budget"; the exact numbers stay here, where they are a dated snapshot and mean
something.

Nothing executable changed. The one finding that could have gone either way is
`src/proc/screen.h`, which claimed its destructor gives the screen and the keyboard back "as
politeness" — `~ProcScreen` frees its grid and does no such thing. The comment was corrected
rather than the code, because a destructor *cannot* release a claim: releasing one is a syscall
and there is nothing to await with. `~Proc` does it kernel-side, which it has to anyway, since a
killed process runs no destructor of its own.

## Warnings are errors, everywhere

`BRAAM_WERROR` defaults to ON and `-Wshadow` joins `-Wall -Wextra`. It had been CI-only since M0,
which sounds like the same thing and is not: the developer who can still fix the code cheaply is
the one who does not see the failure, and CI reports it to whoever pushes next. `rows_equal` in
`test_shell.cpp` is the proof — the applet retirement deleted its only caller and left an unused
function warning sitting in the tree, invisible to every local build.

`-Wshadow` is the one warning worth adding on purpose here rather than trusting to review. A
coroutine frame keeps every local alive across a suspension, so an inner `Result<usize> r` over an
outer one is not the transient confusion it is in straight-line code: the two have different
lifetimes and the wrong one can be the one that survives a `co_await`. The tree needed no changes
to build clean under it, which is the argument for turning it on now rather than after it would
cost something.

The option stays, because a bisect through old commits with a newer clang meets warnings that did
not exist when they were written, and `-DBRAAM_WERROR=OFF` is the difference between reading that
history and not. It is for that, not for pushing past a warning of one's own.

## 0.2 — A version that names the commit

The version is `0.2.24-35f6924`: a base edited by hand, then a commit count and a short hash
that nobody edits. The hand-edited patch number it replaces was wrong more often than it was
right — it moved when someone remembered, so two archives could carry one name and a banner
could describe a tree from a week earlier. A commit count is not a semantic patch level and does
not pretend to be one; it is an identifier that cannot go stale, and the two numbers in front of
it are still the ones that mean something.

**The count orders builds and the hash identifies one.** Either alone is half an answer. A count
says which of two builds is later and is ambiguous across branches, where two commits share a
number; a hash names exactly one tree and says nothing about age, so `a3f19c2` against `35f6924`
is unreadable without a repository to ask. Together they are sortable by eye and exact, which is
what a bug report pasted out of the boot banner has to be. The hash is `%h` rather than the full
40: it is being read aloud and typed, and git's abbreviation grows on its own when the short form
stops being unique.

**It is taken at build time, for the same reason the archive name was.** M7's `file(STRINGS)`
argument was that editing a header does not re-run cmake; committing does not either, and it is
the more frequent event. So `tools/version.py` runs from an always-run target and writes
`build/gen/kernel/revision.h`, which `version.h` includes — the one generated file in the tree,
and small enough that the alternative (a `-D` on the compile line, fixed at configure time) is
just the stale case wearing a different hat. It rewrites the header only when the revision
changes, so `make` after a rebuild relinks nothing; without that, every build would recompile
three translation units and relink the kernel and a binary to no effect.

**One implementation, two callers.** The kernel gets the string through the generated header and
`release.py` imports `version.py`, rather than each parsing `version.h` with a regex of its own.
That was the shape before, and it was already one regex too many: the archive is named after
what the banner prints, so the two agreeing is the whole requirement.

Three edges are answered rather than left to surprise someone. A tree with no repository — an
unpacked release building itself — is revision `0` with no hash instead of a failure, because the
base still identifies it and a build that dies for want of `git` is a worse trade. CI checks out
with `fetch-depth: 0`: the default shallow clone answers 1 to `rev-list --count`, which is not an
error anywhere, merely a wrong version on every artifact it builds. And a build from a dirty tree
names the last commit, which is the honest limit of a hash rather than a bug — an uncommitted
change has no identity to print, and a `-dirty` suffix would make every working build look
suspect while saying nothing about what changed.

## One program model — retiring the kernel applet

There is one kind of program now. `src/prog/` is gone, the program registry with it, and every
command in the system is a wasm binary in an instance of its own with a 16 MB cap, a descriptor
table nobody else can name, and a kill switch. `kernel.wasm` went from 236,965 bytes to 168,804
— a quarter of it was userland — and the boot archive from 47 KB to 379 KB, which is the same
code paying §4.4's duplication cost instead.

**Why the applet had to go rather than stay as a fast path.** It was not a tier so much as a
second program model. Two models meant two `Args` types, two `io.h`s, and `cat` written twice;
they had already begun to diverge, since only one of them could be tested by the in-wasm suite
and only one of them had a memory cap. The tiering argument in §4 is that isolation is chosen by
trust — but nothing in `src/prog/` was more trusted than `wc`, it was merely older. Keeping a
tier for "programs we happened to write first" is not a trust boundary, and it was the tier with
nothing between a bug and the kernel's heap.

**What the ABI cost.** M8 fixed the syscall table at eight and wrote down the rule that keeps it
honest: every operation has a caller, because a syscall nothing calls is an ABI nothing tests.
Moving thirty programs across meant the table had to grow to whatever they actually reached for,
and it roughly tripled — the filesystem's `stat`, `list`, `mkdir` and `remove`; `sleep`; the host
services behind `curl`, `date`, `df`, the clipboard and the file picker; and a terminal family
for the two full-screen programs. Every one of them has a caller in `src/cmd/`, and three
programs got none at all: `pwd` reads a new `/proc/cwd`, `mount` reformats `/proc/mounts`, and
`version` is a constant. Publishing a line of text under `/proc` is cheaper than an operation and
leaves `cat` and `grep` able to read it, which is the same argument M7 made for `/proc` existing.

**Descriptors did most of the work.** The alternative to a `fetch` family, a socket family and a
picker family was to make each of them hand back a descriptor, and let `read`, `write` and
`close` serve all three. That saved perhaps six operations, but the reason to prefer it is what
happens on `^C`: the process's handle table dies with the process, `JsRef`'s destructor releases
the externref slot, and the socket closes with no code written for it. The M6 and M8 machinery
composed exactly as designed, which is the strongest evidence either was designed right.

**The terminal is where the cell grid paid off.** A full-screen program cannot be given the
kernel's grid — it is in another address space — so it paints a grid of its own and blits the
damaged rectangle across with one syscall, cursor included. `src/ui/` became a library over a
`Grid` rather than over the kernel's screen, which is what let `Pane`, `TextBuf` and `TextView`
link into `less` and `edit` unchanged; the kernel no longer links it at all. Had the terminal
been a byte stream this would have been an escape-sequence dialect instead, and §2.3 would have
been paying for itself in the wrong direction.

**Both claims are the kernel's, not the program's.** `KeyClaim` and `ScreenEnter` create a
`KeyInput` and a `FullScreen` on the process's kernel-side record, and `~Proc` destroys them. A
killed tier-2 process runs no destructor of its own, so a program that had taken the screen and
then met `^C` would otherwise leave the shell painting into a grid it does not own. The
destructor that does it is the one M8 already wrote for dropping the instance.

**The step protocol grew a token, which is the one place M9 was at risk.** `chat` listens to a
socket while it reads what is typed, so a process needed two tasks, and two tasks need two
syscalls outstanding. The process side was the easy half — a four-slot waiter table, and
`_resume` already took a token. The kernel half was not: it had assumed one call per process, so
`Sys::Stage` was one reused buffer (the second call would overwrite the first before its server
read it) and the proxy task performed the syscall itself (a socket read that never completes
would have starved the keystroke behind it). So the staging block moved into the call record, and
the proxy split into a stepper and one server job per call. The host now hears which call a step
answers rather than remembering the last, which is a small improvement in isolation on its own.

`chat` gets something back for it: as a binary its descriptors live exactly as long as it does,
so the receiver writes to stdout like anything else and `chat > log` finally captures what
arrives. As an applet it had to write to the screen, because the pipe it would have used belonged
to a job that might already be freed.

**What the tests lost, and where it went.** The in-wasm suite cannot step an instance, so with no
applets left it can drive only the six builtins. That is enough for more than it sounds — a
builtin is an ordinary pipeline stage, so `jobs | help` is a real two-stage pipeline, and the
boot-cost guard and the heap-leak check never needed a program at all. What it cannot do is watch
a program run: the filters, `^C` reaching a running pipeline, typing into a job's stdin, `fg`
waiting. All of that moved to `test/run.mjs`, where the stages are the real binaries. The suite
is a worse unit test and a better system test than it was, and `test_prog.cpp` is gone.

**Smaller decisions.** The op word's upper bits became the operation's argument rather than
specifically a descriptor, so `open` carries its flags there and a payload is only ever the
operation's data; `PROC_ABI` went to 2 for it, which is what the field is for. `Sys::Stage` is
capped at 1 MiB — the largest blit there can be — because a hostile binary may call it directly
and an uncapped `heap_alloc` is an interesting thing to hand one. `/bin` and `/share` are two
re-rooted views of the one bundle, so `BinFs` is deleted and `/usr` with it: M5 promised that
when programs became binaries the mount would change and nothing above it would, and that is how
it read. `help` lists the builtins and then `/bin`, taking its usage lines from
`/share/help`, because the kernel no longer holds a usage string for a program it has never run.
And `src/bin/` became `src/cmd/`: it holds C++ sources, and "bin" only ever distinguished it from
`src/prog/`.

## 0.1.0 — Packaging

`make release` packs `build/web/` into `build/braam-<version>.zip`. The version string, which
had read `0.1.0-m7` since M7 and predated two milestones, becomes `0.1.0`: it names the archive
now, so a stale one would be a stale release, not merely a stale banner.

There was nothing to build. `build/web/` has been a complete deployment since M0 — every URL in
it resolves against `import.meta.url`, so the tree works at any path — and M7 made assembling it
a target of its own. What was missing was one archive to hand to a web host, and the whole of it
is `tools/release.py` beside `pack.py`, a custom target beside `serve`, and two lines of
Makefile.

The archive nests under `braam-<version>/` rather than unpacking loose. A zip that unpacks loose
is unpackable only into a directory the deployer prepared and named; a versioned one can be
unpacked in a web root as it is, two releases never collide on disk, and the directory says which
one is serving. It costs the deployer a `mv` when the URL must stay put, which is the smaller
inconvenience and a reversible one.

The version is read out of `src/kernel/version.h` by the script, at run time. Reading it at
configure time with `file(STRINGS)` was the obvious shape and is wrong: editing a header does not
re-run cmake, so the archive would go on carrying the previous version until someone
reconfigured — a silent error whose symptom is a correct-looking file name.

Determinism is three lines — sort the entries, fix the timestamp at 1980-01-01, fix the mode —
and it buys the ability to answer "is what is deployed what I built?" with `md5`. Without it two
packs of one tree differ, so the question can only be answered file by file after unpacking.

Nothing in the archive configures the server, and nothing needs to. Streaming instantiation wants
`application/wasm` and plenty of static hosts do not send it, which `web/worker.js` has handled
since M2 by falling back to a buffered instantiate. That fallback is what makes "copy it anywhere"
true, and it is why there is no `.htaccess` in the zip to go stale against a host that never
reads it.

`LICENSE` travels with the site. The zip is a copy of the software in the sense the MIT text
means, and the notice is 1 KB.

## M9 — Liveness isolation

`while(1){}` is killable. A binary can ask to run in a Web Worker of its own, and a process
there is ended by `worker.terminate()` rather than by asking it to stop — which is the one thing
M8's isolation could not do, and the reason Concept.md §4.2 exists. 236,965 bytes of
`kernel.wasm` against an unchanged 256 KiB budget: **93 bytes**, which is the headline.

`Concept.md` is amended in five places — the §3 diagram, §4's tier prose, §4.2, §4.3 and §4.4,
and Appendix B — and none of them is structural.

### The ABI did not have to move, and that is the whole result

The obvious reading of M8's §4.3 is that tier 3 breaks it. `sys` is synchronous and returns a
value; a worker boundary has no synchronous direction, because §1 rules out `SharedArrayBuffer`
and therefore `Atomics.wait`. The conclusion looks like "tier 3 needs a second ABI", which would
have meant two process runtimes, two sets of binaries, and a tier that userland could see.

It does not, because the question is not *how does a process reach the kernel synchronously from
another thread* but *does it have to reach the kernel at all*. Taken one at a time, none of the
four synchronous calls does. `GetPid` is a constant the host already binds into the closure.
`Now` is a clock, and a clock reading shipped with the step plus the worker's own elapsed time is
a better answer than a round trip would be. `Exit` is issued by `status_of` immediately before
returning, so buffering it onto the step's reply is not merely equivalent, it is exact — tier 2
keeps the last `Exit` before the step returned, and so does this. And `Stage` is not a program's
syscall at all: it exists so the *host* can ask for somewhere to copy into, and at tier 3 the
host doing the asking is the kernel's worker, which is on the kernel's thread.

So `src/proc/`, `src/cmd/` and the four exports are untouched, the same `wc.wasm` runs at either
tier, and the smoke test asserts one binary per tier against the same import and export lists.
The protocol between the two workers is the *host's*, not an ABI: both halves of it live in
`web/proc.js`, and `web/procworker.js` is ten lines of wiring with no logic to drift.

`Stage` is answered `0` rather than assumed unreachable. A tier-3 process is the untrusted one
by definition, and "no program calls this" is not a property of a binary somebody else compiled.
Zero is the "no room" answer `proc_syscall` already turns into `NoMemory`, so a binary that calls
it gets a defined answer instead of a hole. Unknown operations are refused locally for the same
reason, and never relayed.

### One message per step, not four

The first sketch had `sys_async` and `Sys::Exit` each post their own message, arguing that a
message port is FIFO so the kernel would see them in the right order. It would have worked and
it was still wrong: a suspension is *always* immediately preceded by exactly one `sys_async`
(there is one outstanding call and `_resume` returns right after it), and `Exit` only ever
precedes a return. Both therefore fit on the reply to the step that caused them. Two messages
per syscall instead of four, no ordering argument to get right, and the kernel-side relay is
straight-line code in one handler — the same two lines as M8's `sys_async` closure, with the
source of the bytes changed.

### The kill needed no kernel code

M8 wrote that a tier-2 stage is a `Task<i32>` like any other and that the destructor is the whole
kill path. That turned out to be literally true across a thread boundary as well: `^C` cancels
the proxy, `~End` calls `proc_kill`, and the host terminates a worker instead of dropping a Map
entry. `jobs`, `kill %n`, `/proc`, the tty pump and the stage epilogue needed nothing, and the
kernel diff is three edits — one line of `exec.cpp`, a tier argument on `proc_spawn`, and four
inline helpers in `sysabi.h`.

The one thing that *did* need writing is easy to miss and would have leaked forever: **the
in-flight step must be failed when the worker is terminated.** An abandoned `HostReq` is freed by
`host_orphan_reply` from `wake()`, and `wake()` only happens if somebody answers — so a request
whose worker no longer exists has to be answered by the code that killed it. At tier 2 this
happens for free, because a queued step still runs and finds the pid gone.

Two smaller cases of the same shape: a worker that errors marks its process crashed and drops its
own link, so `kill` cannot hand a dead worker to the next process; and `kill` after a *normal*
exit is not a kill at all, because `exec` kills every process it spawned, including the ones that
exited. That is where the finished worker goes back to the pool.

### The pool is the capability probe

Nested workers are not universal, and Concept.md already promised that a binary asking for tier 3
runs at tier 2 where there is no worker for it. Making that promise good needed somewhere to find
out, and the pool was already going to exist: one worker is hired at boot with no process in it,
so a `Worker` constructor that throws throws at boot rather than under the first `exec`, and the
first `exec` of a tier-3 binary costs an instantiation rather than a worker start.

The pool saves worker startup and not memory — a process's sixteen megabytes go when its instance
does, not when its worker is recycled — which is worth saying because the opposite is the natural
assumption. Idle workers are capped at two.

### The tier on the wire, and the alternative not taken

`HostRequest` had no room: `flags` held both page counts and `aux` is the pid, which is the one
field that must not share. The tier went into the top nibble of `flags`, with `proc_pack` /
`proc_initial` / `proc_max` / `proc_tier` in `sysabi.h` and a `static_assert` that the page
counts still fit — the same shape as `sys_op`'s descriptor packing, and mirrored in `web/proc.js`
as `abi.js` already mirrors `HostRequest`.

The tempting alternative was to let the host read the tier out of the `braam` custom section
itself, which it can: it holds the module, and `WebAssembly.Module.customSections` is right
there. Two ends reading the same bytes provably cannot disagree. It was rejected because §4 says
*`exec`* picks the tier, and a host that picked it independently would leave the kernel unable to
say what it got — and the kernel is the thing that has to report `126` or `132`.

### `spin`, and a loop the compiler was entitled to delete

`spin` exists to be un-killable by cooperation, and the first version of it did not spin at all:
an infinite loop with no side effects is not required to make progress, and clang deletes it. A
`volatile` counter is the fix and the comment above it is the point of the file. `spin N` runs a
bounded number of turns and exits, so the tier's ordinary path — instantiate, write, exit — has a
program that exercises it without waiting for a kill.

`tail` moved to tier 3 as well, which is how the protocol is *checked* rather than argued:
`tail -n 1 /usr/share/motd | wc` is M8's own assertion, unedited, now a tier-3 process feeding a
tier-2 one through a kernel pipe. It buys that coverage at 0.1 ms per 512-byte chunk, which is
the tier's standing cost and the reason it is a claim a binary makes rather than a default.

### What CI proves, and what it cannot

`test/run.mjs` is a straight-line synchronous driver — microtasks do not run during it, which is
why `step()` grew an explicit completion callback and lost its promise. A real thread does not
fit that at all, so `test/fakeworker.mjs` wires the two halves of the protocol back to back over
queues the driver pumps. The whole protocol runs in CI: bind, step, the syscall relay, the exit
status, the pool, and the tier-2 fallback. Only the thread is fake.

Which means the one thing CI cannot prove is preemption, since Node is as single-threaded as the
kernel's worker. So a looping program is *modelled*: a held step is one that sits undelivered,
which is precisely and completely what the kernel sees of a real one — no reply, no timer, and
nothing to cancel but the proxy. The assertions on top of that are the two acceptance criteria:
`^C` on a held process leaves `[130]`, terminates exactly one worker and leaves no instance
behind; and with one held in the background, `echo` and `jobs` still answer.

The real thread was checked by hand, driving the shipping `web/procworker.js` from
`node:worker_threads`: `{k:"ready"}` on load, a bind and a step returning `SUSPENDED` with a
write of `spin: pid 7, spinning` — which is `GetPid` answered inside the worker with the pid the
host bound — then the reply that sends it into its loop, no answer, and `terminate()` returning
in 2 ms. That is the criterion, once, outside a browser.

`dispose()` gained a handshake for the same reason. It used to terminate the kernel's worker
outright; a nested worker is specified to go with its parent, but a leaked one is a core spinning
for the life of the page, which is too much to leave to a spec this code cannot check. The page
now says so first and terminates on the next turn as the backstop.

---

## M8 — Isolated processes

A program can now be a binary of its own, in a `WebAssembly.Instance` of its own, with an
address space, an `externref` table, a file-descriptor table and sixteen megabytes that belong
to nobody else — and the shell, the pipes, `^C`, `jobs` and `/proc` do not know the difference.
236,872 bytes of `kernel.wasm` against an unchanged 256 KiB budget, plus three binaries of
6–17 KB each.

`Concept.md` is amended in five places: §3.4 (two new exports and the record's new word), §4
(tier selection, and what the unit tests can drive), §4.3 (the ABI as built), §4.4 (the compile
is not streaming) and Appendix B (how the copy actually gets its destination).

### The rule the whole design turns on

**The kernel never calls a process, and the host never calls one while the kernel is on the
stack.** Everything else here follows from that sentence.

The first half is not a choice: wasm has no instruction that reaches another instance, so only
JS can call `_start`. The second half is: JS *could* call `_start` from inside `host_svc`, the
way `test/fakefs.mjs` answers a storage request from inside the import — and that works there
precisely because a reply only queues. A process step is not a reply. It runs a program, and
that program immediately calls back in through `sys`, which allocates, touches the process
table and wakes a token. Doing that on top of a half-finished `HostCall::issue()` is a class of
bug that would show up as heap corruption weeks later.

So one `_start` or `_resume` is a deferred host action, structurally identical to a storage
reply: the kernel's proxy task parks on a wake token, the host steps the instance once the tick
has unwound, and wakes the token with the outcome. In `web/worker.js` that deferral is a
microtask; in the test driver it is an explicit `drain()` between ticks; the stepping code
itself is the same `web/proc.js` in both, because the difference is scheduling and not
behaviour.

Synchronous syscalls run the other way and need none of this. `sys(pid, …)` re-enters the
kernel from JS at top level, exactly as `key()` and `wake()` do, and answers without parking.

### The proxy task is the entire cancellation story

A tier-2 stage is a `Task<i32>` like any other. It spawns the instance, then loops: step, and
when the step reports "suspended", perform the syscall the process is parked on and step again
with the answer. The syscall is performed *by the proxy, in kernel-land*, with the proxy's own
`CancelToken`, against the `Stdio` the job gave it — so a write into a full pipe is
`Stream::Write`, a read at end of input is `Source::Read`, and `^C` reaches a process through
exactly the awaitables it reaches an applet through.

That is why M8 adds nothing to the job runtime, the job table, `/proc` or the tty pump. It is
also why the destructor is the whole kill path: cancelling the proxy unwinds it, and `~End`
tells the host to drop the instance. A killed process never unwinds — its coroutine frames, its
heap and its descriptors go at once, which is the isolation working rather than a shortcut.
That is a strictly better kill than an applet gets, and still not a *liveness* kill: a process
in a loop between syscalls is M9's problem, and nothing here changes that.

### No new import, two new exports

M5 fixed the style at one import per calling convention and M6 held to it. Spawning,
stepping and killing are asynchronous host operations with a request record, which is
`host_svc` exactly, so they are three more of its operations. §2.2 still sanctions two
synchronous exceptions and there are still six imports.

The two new exports are not the host's business at all: they are the process's `sys` and
`sys_async`, forwarded with a pid. That indirection is the capability system §4.1 promised, and
it is twelve lines of `web/proc.js` — the closure is built per instantiation with the pid
written into it, so *process 7 holds no function that says 3*. The second acceptance criterion
is not a check that runs; it is a shape the ABI has, and what the smoke test asserts is the
shape: the module's imports are `env.memory`, `kernel.sys`, `kernel.sys_async`, and `sys` has
no argument a pid could go in.

`HostRequest` gained one word, `aux`. The alternative was to overload `flags` and write the
page counts into `result_lo` on the way *out*, which would have made a reply field an argument
field and saved four bytes.

### Memory is imported, so the cap is the kernel's

`-Wl,--max-memory=16777216` would also make `memory.grow` fail at 16 MB, and it would be the
*binary's* number. `--import-memory` with no declared maximum puts it the other way round: the
module says only what it needs to start with, and the host supplies
`new WebAssembly.Memory({initial, maximum: 256})`. A binary cannot ask for more by being
compiled differently. That is what §4.1 means by an rlimit without cgroups, and `hog` is in
`src/cmd/` to demonstrate it: it takes 64 KiB at a time until the allocator says no, gives one
span back so it has somewhere to put the coroutine frames that report the answer, and asks
`memory.grow` for one more page.

### The metadata is stamped after the link, not compiled in

`exec` reads the tier out of a `braam` custom section, which was to be a
`__attribute__((section(".custom_section.braam")))` global. It never reached the object file —
`used` keeps the compiler from dropping it but does not make it a custom section here — and
rather than fight the toolchain, `tools/stamp.py` appends the section after the link.

That turned out to be the better place anyway. The section carries `initial_pages`, which has
to agree with `-Wl,--initial-memory`, and the stamper is invoked from the same four lines of
`src/cmd/CMakeLists.txt` that set the link flag. A number that must match another number should
be written once.

### What the port of `wc` and `tail` proves, and what stopped `echo`

The third criterion is "userland behaviour is unchanged", and the honest way to check it is to
move a program and leave its tests alone. `wc` and `tail` moved to `/usr/bin`; `echo 'a b' | wc`
still prints `1 2 4`, `wc < notes` still prints `2 2 8`, and `curl /hello.txt | wc` still prints
`1 2 9` — M4's, M5's and M6's assertions, unedited, now running an instance through a
pipe, a redirection and a fetched body.

`echo` and `sleep` were meant to move as well, and could not. The in-wasm unit tests drive
both — `echo` is `test_shell`'s workhorse and `sleep` is `test_jobs`'s only timer — and
`tests.wasm` cannot run a tier-2 program *at all*: stepping an instance means returning to the
host, and `run_tests()` returns to the host exactly once, at the end. Porting them would have
meant rewriting two of the load-bearing unit tests around programs chosen to suit the harness,
which is the tail wagging the dog. The constraint is real and general, so it is written into
§4 rather than left as a note here: whatever `test/unit/` runs has to stay an applet.

Two assertions in `test_shell` did have to move off `wc`, and both got better for it: one now
checks a pipeline by its output rather than by counting it, and the other checked a
hand-maintained count of programs whose names contain an `e`.

### The syscall table, and the isolation this does not buy

Eight calls: `exit`, `getpid`, `now` and `stage` synchronously, `write`, `read`, `open` and
`close` asynchronously. Every one has a caller in `src/cmd/`; a `sleep` syscall was written and
then removed when `sleep` stayed an applet, because a syscall nothing calls is an ABI nothing
tests.

`stage` is the odd one, and it is the host's rather than a program's: the kernel cannot be
handed a buffer it did not allocate, so before copying a payload across the closure asks for
one. That is Appendix B's `Uint8Array.set` plus the one thing Appendix B did not mention.

A descriptor is an index into the process's own table, so a number one process holds means
nothing in another. Paths are not: `open` resolves against the one global cwd with the kernel's
full authority, so M8 isolates address space, memory and descriptors, and does not isolate the
namespace. Fixing that needs a per-process root and a cwd that is not a global, which is a
milestone's worth of work in the VFS and not a line in the dispatcher.

### Smaller decisions

- **`panic` is out of line now**, and takes `(ptr, len)` rather than a `Str`. A process has no
  host imports to log through, so it needs a different definition, which means the inline one
  had to go. The first version took a `Str` and cost 2,812 bytes: the wasm ABI passes an
  eight-byte struct indirectly, and there are a hundred call sites. Two scalars cost 55.
- **`stage()` in the job runtime takes a built `Task<i32>`** rather than a `Program *`, because
  a tier-2 body needs its pid and `sched_spawn` only hands one back once the task exists. The
  pid goes into the `Executable` the job owns, and the proxy reads it at its first resume — a
  tick later, since a `Task` is lazy.
- **The scheduler's name for a stage is `argv[0]`**, not `Program::name`, which a binary does
  not have. It is a view into the job's word store, which outlives every stage.
- **`help` lists `/usr/bin` too.** A program need not be in the registry to be a program, and
  what tier a name runs at is not something the listing should say.
- **A binary is re-read from the VFS on every `exec`.** The host caches the compiled `Module`
  by path, which is the expensive half, but the bytes still cross the VFS and one copy into the
  request record each time. Caching those too wants an invalidation story, and `/usr` being
  read-only is not one that generalises.
- **The bundle is staged rather than packed in place.** The binaries are build outputs and
  `/usr/bin/wc` has to be a plain name, so `build/bundle/` is a copy of `bundle/` with
  `bin/<name>` added, and that is what `tools/pack.py` packs.

## M7 — Depth

A program can take the whole screen and give it back, a job can outlive the prompt that started
it, the scheduler can be read as files, and a host page can put a terminal on itself in six
lines. Five new commands, one new library, and — the part worth stating first — **no ABI
change at all**: no new import, no new export, and `test/run.mjs`'s exact-surface assertion is
untouched. 225,784 bytes of `kernel.wasm`, against an unchanged 256 KiB budget.

`Concept.md` is amended in four places: §3.5 (the layout layer, the keyboard claim, the saved
screen, and re-wrap deferred), §3.6 (the job table, and the scheduler's names), §5.1 (`/proc`)
and §7 (`src/ui/`).

### One receiver, so the pump routes rather than yields

`Channel` has one receiver and a second suspended receiver silently displaces the first. While
a pipeline runs, that receiver is `tty_pump`, and the shell's handshake at the end of
`run_line` — cancel the pump, wait for its report — depends on it. An editor that simply did
`co_await keys().recv()` would displace the pump, take `^C` with it, and race that handshake.
The bug would not look like a bug; it would look like the keyboard occasionally going dead.

So a full-screen program does not receive keys, it **claims a route through the pump**:
`KeyInput` gives it raw keys with the echo and the line discipline turned off, and the pump
`try_send`s into its ring. One mechanism covers three cases that looked unrelated at the start
of the milestone — the editor, the pager, and `fg`, which needs the *cooked* bytes to go to a
different job's stdin and gets `InputClaim` for it.

`^C` is deliberately not routed. An editor could reasonably want it, but a program that has
taken the entire screen and stopped answering must stay killable by the key that kills
everything, and that is M4's acceptance criterion as much as it is a safety rule. `edit` quits
with `^Q` for the same reason, and ^C throws the buffer away.

The claim is RAII, which is what gives the route back when a claimant is killed rather than
asking it to be polite. M7 made it restore whatever was in force before, on the reasoning that
claims should nest; that was wrong, and "One claimant, named by pid" above says why and replaces
it with a single holder that refuses the second.

### The single waiting slot decides the shape of `less` and of `fg`

`CancelState::waiting` is one slot, so a task cannot be parked on a pipe and on the keyboard at
once. This is the same constraint that pushed pipelines into independent scheduler jobs in M4,
and it shows up twice more here.

`less` therefore reads its input to end-of-input *before* it paints anything. A real pager is
lazy, and this one cannot be without becoming two coroutines and a refcounted shared block —
the `chat` pattern, which is 100 lines and a class of lifetime bugs to buy laziness nobody
asked for in a tab. It does claim the keyboard first, before reading, so that what is typed
while a slow pipe fills is queued for the pager instead of being echoed at the shell.

`fg` never awaits keys at all. It parks on the adopted job's completion token and lets the pump
do the routing, which means ^C cancels *`fg`* — and `fg`'s destructor passes that on to the job
it adopted. Cancellation propagating through a destructor rather than a branch is the same
trick `run_line` and `chat` use, for the third time.

### A background job is the same `Job`, with the reaper in the shell's place

`Job` was already a refcounted heap block, already outliving the frames that point at it,
already carrying its own `done` channel — M4 built it that way for a different reason, and
backgrounding needed almost none of it changed. What a background job needs is somebody to
stand where `run_line` stands: collect the reports, record the status, drop the last reference.
That is `reaper`, forty lines.

Three details are not obvious. `CancelAll` gained an `armed` flag rather than a branch around
it, because it must still fire when setup fails partway. The command text is *copied* into the
table entry — `run_line` receives a `Str` into the shell frame's line buffer, which is gone
before `jobs` ever runs. And the entry holds a reference of its own, because `fg` and `kill`
reach the pipes and the pids through it long after the shell moved on.

A background job gets no pump and its stdin is closed at once. Giving it the keyboard would
mean deciding which of several running jobs a keystroke belongs to, which is what a foreground
group is for; end-of-input is what a shell does with `&` anyway.

There is no `bg` and no `^Z`. Suspending a running coroutine at an arbitrary point is the
resume-side twin of `CancelToken`: every awaitable would have to consult a stop flag and every
awaitable would have to be resumable without an event. That is a milestone, not a command, and
a stub that only ever printed "not supported" would be worse than its absence.

Finished jobs are announced by the shell before the next prompt rather than wherever they
happen to end, which would otherwise land in the middle of a line being typed. It is a
coroutine of its own so that its locals stay out of `shell()`'s frame, which has a size class
to fit inside — the same reason `boot_filesystem` is one.

### `Pane` writes cells, so the kernel grew `screen_touch`

`screen_cells()` has been public since M2 and writing through it marked nothing damaged, so the
renderer would not have repainted what a pane wrote. The alternative was to route every pane
write through `screen_put`, which moves the global cursor, defers wrapping, and scrolls the
whole grid when it reaches the bottom — three behaviours a clipped rectangle must not have. One
new function is the smaller change, and `screen_touch(x, y, w, h)` is what any later direct
writer will want too.

A pane fills with blank cells — codepoint 0 — rather than spaces, since 0 is what the grid means
by empty and it still carries the pane's colours; that is what makes a reversed status line one
`fill_row` rather than a loop of spaces.

`FullScreen` copies the grid to a heap block and copies it back from its destructor. If the
geometry changed while the program ran, it clears instead: the snapshot describes a grid that no
longer exists, and pasting it back would be worse than blanking. That path is tested, because it
is exactly what a window resize during `edit` does.

### `/proc` is flat, and generated at open

`BinFs` set the pattern in M5 and `ProcFs` follows it: a generated read-only filesystem, so
`cat` and `grep` are the introspection tools and there is no second interface to keep in step
with the first.

Two departures from Linux. The tree is flat — `/proc/42` is a file, not a directory — because a
process here has one line of state and a directory level would hold exactly one file; if a
process ever grows `cmdline`, `cwd` and `fds`, that is the moment to add the level. And content
is produced at `open` into a heap block that the descriptor owns, so a file read in two blocks
cannot describe two different moments. `BinFs` could regenerate per read because a usage line
never changes; `meminfo` changes on every allocation, including the ones `cat` itself makes.

The scheduler had to give up a little: a `Str name` on its private job record, set at
`sched_spawn`, and a `sched_procs` snapshot. The name is a view, so it must outlive the task —
a literal or a `Program::name`, never a local — and the header says so. `sched_procs` returns
nothing while `tearing_down` is set, for the reason `find_job` does: `jobs[]` holds freed
pointers while `~Sched` walks it.

### The embedding API is an extraction, not an invention

`index.html` had 190 lines of module in it and nothing there could be imported. Everything below
it was already dependency-injected — `makeFsImports`, `makeSvcImport`, `makeImports` all take
their backends as arguments, which is what the test fakes have been proving since M5 — so the
page was the only layer that was not.

`web/braam.js` exports one function, `mount({canvas, ...})`, returning a handle with `focus()`
and `dispose()`. The keyboard listener moved from `window` to the canvas, which is the one
behavioural change: an embedded terminal shares its page, and two of them must not both read
the same keystroke. `dispose()` exists because a host that swaps views has to be able to let go
— it terminates the worker and drops every listener and observer.

The worker stays one kernel per worker. That is not a limitation to fix later; it is the
isolation model M8 builds on, and two terminals on a page are two workers that share nothing
but the origin's storage. `web/embed.html` runs exactly that, with a different palette on the
right-hand one to show that a theme is an embedder's choice.

`E_PERM`/`E_IO`, which the page had been re-declaring as literals, now come from `abi.js` like
everything else on the wire.

### What the browser found, that no test could

The unit cases and the smoke test drive `kernel.wasm`; nothing drives the shipping page. So the
embedding API was checked in a real browser — headless Firefox, a page reporting back through
the HTTP log, and a screenshot taken after a deliberately slow subresource delayed the load
event. Two defects came out of it, both older than M7 and neither reachable from Node.

**Boot waited on `navigator.storage.persist()`, which is not always quick.** M6 chose to block
on it, reasoning that reporting the wrong durability is worse than a tick of delay. That is
right about the trade and wrong about the number: Firefox took over five seconds to answer, and
boot waited the whole time behind a blank canvas. The page now sends a provisional best-effort
answer after a 250 ms grace period and the real answer when it arrives, and the late one
corrects `OpfsStore.persisted` rather than boot — so `df` is right from the moment the browser
decides. With two terminals on a page it was worse than slow: a second `persist()` issued while
the first is outstanding did not settle until the first had, so the second kernel never booted
at all. Persistence belongs to the origin, so the request is now made once per page.

**`build/web/` went stale on a web-only edit.** Assembling it was a `POST_BUILD` step of the
`kernel` target, so changing nothing but a `.js` file left `make serve` serving the previous
copy — which is exactly how the first defect was nearly missed, since the fix under test was not
the code being served. It is its own always-run target now.

### Two things that did not land

**Re-wrapping logical lines on resize.** §3.5 promised it to "the layout layer in M7", and the
layout layer is the wrong place for it: re-wrapping needs to know which rows are continuations
of the row above, which is a bit `screen_put` must set as it writes — inside the grid, below
everything a pane can see. It also collides with `LineEditor`'s anchor arithmetic, which infers
scrolls by comparing where a write should have ended against `cursor_y`. Doing it properly is
scrollback's problem and it will arrive with scrollback.

**A pane of `chat`'s own.** M6's notes said the layout layer is where an interactive program
gets one. It is, but only for a program that owns the foreground: `chat`'s receiver is a
detached task that outlives its parent, and painting into a pane whose owner is gone is the same
use-after-free as writing into a pipe whose `Job` is gone. It still writes to the screen.

---

## M6 — Host services

The tab reaches outside itself: HTTP, WebSocket, the clipboard, the user's own disk, and a
clock that can name a day. Seven new commands, one new import, one new export, and the
`externref` table Concept.md §3.7 has been promising since M0. 181,545 bytes of `kernel.wasm`,
against an unchanged 256 KiB budget.

`Concept.md` is amended in six places: §2.2 (no third synchronous exception, and why the new
export is not one), §3.4 (`host_fetch` becomes `host_svc`, and `ref` joins the exports), §3.7
(the table is real, and it points the other way), §5.1 (`/mnt/import`, and the bundle stays an
archive), §5.4 (the escape hatch exists) and §7 (`src/svc/`).

### The table is deposited into, not read out of

§3.7 described "a `WebAssembly.Table` of `externref` that wasm indexes into and JS populates".
Half of that is wrong, and the half that is wrong is the half about JS.

`__externref_t` and the `__builtin_wasm_table_*` builtins work exactly as hoped —
reference-types is on by default for `wasm32-unknown-unknown`, so a `static __externref_t
table[0]` compiles and links with no extra flag. What does not work is getting JS to that
table: `import_module` and `import_name` are function attributes, and clang rejects them on a
table outright. An imported table is not available, and exporting the module's own table means
teaching wasm-ld to export a table symbol.

Neither is needed, because an import may take an `externref` *parameter* and an export may
take one too. So the traffic runs the other way and JS never touches the table at all:

- The kernel reserves a slot before it issues the request, and puts the number in the record.
- The host resolves its promise and calls `ref(slot, obj)`, the one new export.
- To use the object later, the kernel reads the slot and passes it as `host_svc`'s fourth
  argument. The host is handed the object; it never asks for it.

This is better than the sketch rather than merely equivalent. A host that cannot index the
table cannot reach an object the kernel did not deliberately pass out, which is the property
§4.1 wants from per-instance tables in M8 — and it comes for free, since a module-defined
table is part of the instance.

`ref` is an export, so §2.2 is untouched: the rule is about imports returning data, and this
returns nothing.

### One request record, two interfaces

A service call needs precisely what M5 built for storage: a heap-owned record that outlives a
cancelled awaiter, an orphan list, the two-phase reply for a variable-sized answer, and the
register-then-issue ordering in `await_suspend`. Writing a second copy of it would have been
about 150 lines, a second orphan list, a second reaper chained off `wake()`, and a second
`Request` class in JS.

So `FsRequest`/`FsReq`/`FsCall` moved to `src/kernel/hostcall.h` as `HostRequest`/`HostReq`/
`HostCall`, tagged with a `HostIface` that picks the import in `issue()`, and `FsCall` and
`SvcCall` are one-line subclasses that name the op enum. `path_ptr`/`path_len` became
`arg_ptr`/`arg_len`, because a URL and a clipboard string are not paths. `web/abi.js` is the
same move: `Request`, the field table, the error numbering and `statusOf` now live there, and
`fs.js` and `svc.js` both import them.

The risk in a refactor like this is that it quietly breaks the cancellation path nobody looks
at. `test_hostfs` is exactly that test and it did not move, which is the reason to do the
refactor in the same milestone rather than the next one.

The record gained one word, `ref`, and the ownership question that comes with it. A borrowed
slot — `WsSend` naming a socket the caller holds — is just a number. A slot the *reply* fills
is owned by the record, not by the frame: `reserve_ref()` puts a `JsRef` inside `HostReq`, so
a request cancelled before it is issued frees the slot along with the record, and one
cancelled after it is issued keeps the slot alive at the address the host still holds.
`test_svc` exists for that one case.

### No `host_svc_sync`

The wall clock is the near miss. `Date.now()` is as synchronous as `performance.now()`, and a
`host_clock()` import would have been three lines. It would also have been §2.2's third
exception, and the section says plainly that at three it stops being pragmatism.

Making it an operation on the asynchronous ABI costs one round trip in a program that runs
once and prints a line. That is the right trade every time, and the fact that it was even
tempting is why the rule is written down.

### Two waits mean two jobs, and the child outlives the parent

`chat` reads the keyboard and the socket at once. `CancelState::waiting` is a single slot, so
that is two jobs — the same conclusion M4 reached for pipeline stages, and the second place
§3.6's structured concurrency is put back by hand from a destructor.

What is new is that the child can outlive the parent. `run_line`'s `CancelAll` cancels its
stages, but a cancelled coroutine does not unwind until the scheduler resumes it, which is a
tick or two later. In M4 that was harmless: the pump holds a reference to the refcounted `Job`,
so everything it touches is alive for as long as it is. `chat`'s receiver has no such handle —
a program is given a `Stream`, not the thing that owns the pipe behind it. Cancel `chat` in
`chat url | tee log` and the receiver could wake up to write into a pipe whose `Job` has
already been freed.

So the receiver touches nothing the parent owns: the session is refcounted and holds the
socket, and incoming lines go to `screen_write` — a global, like `boot.cpp`'s diagnostics.
The cost is that `chat > log` does not capture what arrives. For an interactive program that
is close to right anyway, and M7's layout layer is where a program gets a pane of its own.

This is the second time a child-group awaitable would have made the problem disappear. It
needs intrusive queue links in `Waiter` first, which is the same work a channel with two
blocked senders needs; the note in CLAUDE.md stands, with one more reason behind it.

### CORS is the first wall, so `curl` says so

A relative URL resolves against the page, which is why `curl /index.html` works with no network
at all and why the acceptance criterion can be met offline. Anything cross-origin needs the
server to send `access-control-allow-origin`, and `fetch` reports a CORS refusal and a dead
network identically, as a `TypeError` with nothing in it.

That reaches the kernel as `Error::Io`, and "curl: https://…: i/o error" would send someone
looking at their connection. The diagnostic appends "(a cross-origin URL needs CORS)" on `Io`
and nothing else, which is the one hint that is right most of the time.

### The clipboard, the picker and the download live on the page

Three of the thirteen operations cannot happen in a worker: `navigator.clipboard` is
main-thread only, `<input type="file">` and an `<a download>` click need the DOM. `web/svc.js`
keeps a map of pending ids and relays those three to the page over `postMessage`.

None of that is visible from the kernel, which is the point of §2.2: a service operation is a
token whether the answer comes from `fetch` in the worker or from a file picker two threads
away. The picker opens inside the transient activation of the keystroke that ran the command,
so `import` needs no button of its own; the page reads each file with `arrayBuffer()` and posts
the bytes down, so the slot holds a plain array and reading a file back needs no further relay.

### Reading the clipboard needs a gesture the command cannot have

`pbpaste` failed in a browser with "permission denied", and the reason is not a bug that can be
fixed where it appeared. `navigator.clipboard.readText()` is only permitted from inside a
user-gesture handler. Our request reaches the page over `postMessage`, which is to say after
the keystroke's handler has returned — so the call is *never* inside a gesture, no matter how
promptly it arrives. Safari refuses outright, Firefox does not expose the API to page content,
and Chrome prompts. `pbcopy` is unaffected because `writeText()` is far more permissive.

Transient activation would not have saved it either: the five-second window governs things a
page *initiates*, and a clipboard read is checked against the handler it is in.

The escape is that a **paste is the gesture**. The `paste` event delivers the text with no
permission at all, everywhere. So `ClipWait` is a fourteenth operation: `pbpaste` tries
`clip_read()`, and on `Perm` prints "press ⌘V or ctrl-V to paste" on stderr and parks until the
page's `paste` listener answers. Where `readText()` is allowed, nothing is printed and nothing
changes.

Three details fall out of it. `Ctrl+V` joined the reserved set in `web/keys.js`, since a
keystroke the kernel eats produces no `paste` event — on macOS `⌘V` was already left alone,
because `consumes()` never claims a `metaKey` chord. The held-text buffer in `web/svc.js` exists
because the reply is sized twice like any other, and a paste cannot be asked for again: the same
shape as a WebSocket message waiting at the head of its queue. And `^C` while parked is the
ordinary orphan path — the page's waiter stays armed, the eventual paste replies to a token
nothing waits on, and `host_orphan_reply` reaps the record; the smoke test walks exactly that.

### A response body streams; a message does not

Two reply shapes, for two different unknowns. A response body is read with `writeSome` — as
much as fits in a 512-byte buffer, with the host keeping the remainder — because a body has no
size worth asking about and 512 bytes is the allocator's top size class (§8.2). A WebSocket
message uses the two-phase reply instead, because a message is atomic: splitting one across
two reads would lose the boundary that makes it a message.

The retry that the two-phase reply implies is why a `Fetch` reply deposits its object *before*
reporting the header size. A retry arrives with the object already in the slot, so the host
skips the request and only rewrites the headers — otherwise asking for more room would issue
the fetch a second time.

### Proving the browser half without a browser

`test/fakesvc.mjs` mirrors `test/fakefs.mjs`: canned routes, a loopback socket, a clipboard
variable, a fixed set of picked files, all answered from inside the import. That covers the
kernel, and it deliberately does not cover `web/svc.js`, which is the code a user actually
runs.

So `tools/wsd.mjs` is a real WebSocket server — the RFC 6455 handshake and frame codec in
about a hundred lines of dependency-free Node — and `make serve` starts it beside the static
server. Two tabs running `chat ws://localhost:8081` talk to each other with no internet, which
is what the second criterion means and what the loopback fake cannot show.

### Smaller decisions

- **`/mnt/import` is a directory, not a mount.** The picker hands over bytes. A read-through
  `Fs` over `File` objects would exercise `JsRef` more thoroughly and would not make `cat`
  work any better.
- **`Drop` is fire-and-forget.** Releasing a slot is not enough on its own: an open socket is
  held alive by its own event handlers on the JS side. `JsHandle` says so out loud, with a
  token-less `host_svc` call from its destructor, which is also what closes the socket when
  `^C` destroys the frame holding it.
- **`E` gained `CANCELLED`, `AGAIN` and `CLOSED`.** `web/fs.js` listed only the values it
  reported; a socket that has gone away needs `Closed` to mean EOF to a reader, as it does for
  a pipe.
- **`date` carries its own calendar.** Twenty lines of `civil_from_days` rather than asking the
  host to format, which would have put a locale in the ABI. `-u` prints UTC; without it the
  offset comes back from the host in the reply's `flags`, biased by 1440 to stay unsigned.
- **`export` buffers the whole file.** A download is one Blob, so there is nothing to stream
  into.
- **The `help` grid grew.** Twenty-seven programs no longer fit twenty-four rows, so
  `test_prog` and the smoke test both resize before checking `help`. That the assertion needed
  changing at all is the tripwire working.

---

## M5 — Filesystem

A mount table, four filesystems, an open-file table, redirection that reaches real files, and
seven new commands. This is the first milestone whose state outlives the tab, which is what
makes it the first one where getting the boundary wrong loses a user's work rather than a
frame. 137,867 bytes of `kernel.wasm`, against an unchanged 256 KiB budget.

`Concept.md` is amended in five places, because five decisions here differ from what it said:
§3.4's import list, §3.6's `Fs` sketch, §3.7's `externref` table, §5.1's mount layering and
§5.3's capability struct. Each is argued below.

### Two imports, not ten

Storage needs roughly ten operations. Concept.md §3.4 listed `host_storage_read` and
`host_storage_write`, which suggests one named import per operation, and that is what the naive
reading of §2.2 wants: an import is a syscall, so name it.

The trouble is that the smoke test asserts `kernel.wasm`'s *exact* import list, deliberately —
it is how an accidental libc dependency is caught at link time rather than as a runtime trap.
Ten named imports means that assertion churns on every operation added, and the churn is
noise: nothing about `host_fs_truncate` appearing is a fact anyone needs to review. What is
worth reviewing is a new *calling convention*, and there are exactly two of those.

So: `host_fs(op, token, req)` for everything asynchronous, and
`host_fs_sync(op, handle, ptr, len, off) -> i32` for §5.2's sanctioned exception. That is also
the shape §4.3 already fixes for the M8 process ABI — `sys` and `sys_async` — so the boundary
userland crosses in M5 is the one it will keep crossing when programs become instances.

The cost is an untyped op number in place of a symbol, and it is real: a mismatch between
`FsOp` in `src/fs/hostfs.h` and `OP` in `web/fs.js` is a wrong answer rather than a link error.
That is why `test/fakefs.mjs` imports its constants and its encoders *from* `web/fs.js` rather
than restating them — the two sides of the wire cannot drift without the tests noticing.

### A request outlives its awaiter

`wake(token, ptr, len)` carries two words, and a directory listing is not two words. Something
has to own a buffer the host can write into, and the obvious answer — export `kalloc`/`kfree`
and let JS allocate — is wrong in a way that took a moment to see.

The problem is cancellation. `^C` during `ls /home` destroys the frame that issued the request.
If the reply buffer lived in that frame, or was owned by the host on the frame's behalf, the
promise resolving a moment later would write into freed memory. Every awaitable in this system
deregisters in its destructor precisely so that destroying a suspended frame is safe (§8.1),
and a raw address handed across the boundary defeats that.

So a request is a heap record with its own path and reply buffers, and `FsCall`'s destructor
does not free it. If the reply has landed, the record goes; if it has not, the record is
*orphaned* — moved to a list and left alive at the address the host holds. The reply is what
finally reaps it, which is why `sched_wake` now returns a bool and `wake()` routes an
undelivered token to `fs_orphan_reply`. An unclaimed token was previously ignored; now it is
the one signal that says a record is safe to free.

Two flags decide a record's fate, and both are easy to get wrong. `issued_` says the host was
given the address at all — a request cancelled before it reached the import has nothing to wait
for and must be freed, not orphaned, or it leaks silently. `done` says the reply landed, and it
cannot simply be set on the resume path: a cancellation also resumes. `sched_cancel` is the only
thing that sets `Waiter::cancelled`, so `done = issued_ && !w_.cancelled` is exactly right,
including the case where a cancellation arrives *after* the reply and the request is finished
with regardless. `test_hostfs` exists for this and nothing else.

This also keeps the export surface where §3.4 put it: `init`, `wake`, `tick`, `key`, `resize`
and `memory` is what M5 ships, exactly as M2 did.

### Naming is asynchronous; an open file is not

Concept.md §3.6's `Fs` was `read`/`write`/`list`, all returning `Task`. Splitting it instead by
*when the work can happen* is the single change that makes the rest of the milestone simple.

`stat`, `list`, `open`, `mkdir` and `remove` may need the host, so they park. `read`, `write`,
`size`, `truncate` and `close` act on an open handle, and §5.2 says those are genuinely
synchronous on OPFS. Following that split gives redirection its shape for free: a job opens
its files at setup, before any stage is spawned, and from then on `Stream::Write` is a plain
call. The file-backed `Stream` and `Source` never park, so their `park` hook is null and the
whole retry-on-`Again` path is dead code for them.

It also means a failed open stops the command before it produces side effects, which is what a
shell does and what M4's placeholder refusal was standing in for.

`read` fills a caller's buffer rather than returning `Bytes`. That is the same argument the
pipe made in M4: a `Span` is a pointer and a length, and nothing below the VFS owns a buffer
the caller may keep.

### /bin is a filesystem

M4's `ls` listed the program registry and its comment promised that M5 would replace the body
with a walk of the mount table. Doing only that would have left `/bin` an empty directory —
programs are in-kernel coroutines until M8 — and the registry reachable only through `help`.

`BinFs` is about sixty lines and fixes both: the registry *is* a read-only filesystem mounted
on `/bin`, `ls` really is an ordinary directory walk, and `ls /bin | grep hel` still means what
it meant in M4. A file there reads as the program's usage line, because that is the only thing
about a program there is to read. When M8 gives programs binaries, the mount changes and
nothing above it does.

It lives in `src/user/` rather than `src/fs/` because the registry does, and `braam_fs` must
not depend upwards.

### The bundle is an archive, not the Cache API

§5.1 pairs `BundleFs` with the Cache API. The Cache API stores `Request`/`Response` pairs,
which is a good fit *once something is producing them* — and that is `fetch`, which is M6. Using
it now would mean pulling M6's import forward to serve a tree that never changes after the
build.

Instead the worker loads one `bundle.bin` beside `kernel.wasm` and hands the bytes over through
the `Bundle` operation, and `BundleFs` unpacks it in memory. One request instead of one per
file, no new import, and the format is small enough that `tools/pack.py` and
`src/fs/bundlefs.cpp` are each about eighty lines. The smoke test is given the archive the
build just produced, so the packer and the reader are checked against each other rather than
each against its own reading of the format.

### Two round trips, not one enormous buffer

A reply whose size is not known in advance — a listing, the bundle — is asked for twice: the
first attempt reports the room it needs and writes nothing, and the kernel retries with a
buffer that size. The alternative is a buffer big enough for anything, and the allocator makes
that expensive in a specific way: 512 bytes is the top size class and a byte more costs a whole
64 KiB span (§8.2). A directory that fits in a block — nearly all of them — costs one trip; one
that does not costs two, and only pays for a span when it genuinely needs one.

That is also why files are read `FS_BLOCK` = 512 bytes at a time, and why `file_read` stages
through a stack buffer rather than a `String`: reading is synchronous, so the buffer does not
have to survive a suspension, and a 512-byte chunk lands exactly in the top size class on its
way into a pipe.

### One open handle per file

§5.2 said the open-file table should refuse a second *writer*. It refuses a second open of any
kind, because that is what OPFS actually enforces: `createSyncAccessHandle()` takes an
exclusive lock and a second handle fails whatever mode it asks for.

The looser rule would have worked on `MemFs` and failed on OPFS, which is the worst outcome —
a program that behaves differently depending on which mount its path landed in. `cat a a` is
refused as a result. That is a real restriction and an odd command, and the honest rule is
worth more than the odd command.

### The working directory is a global

A program is `Task<i32>(Args, Stdio)`. There is nowhere to put a per-process cwd until §4.3's
ABI gives a process a context, which is M8. With one shell running, a global and a per-process
cwd are indistinguishable, so `cd` mutates one `String` in the VFS and the difference is
deferred rather than designed around.

The shell starts in `/home` rather than `/`, which is what makes `echo hi > notes` land
somewhere that survives a reload without the user having to know that it must. The prompt stays
`$`: putting the cwd in it is layout work, and it belongs with M7 rather than with a change
that would churn every screen assertion in the tests.

### Boot happens in the shell, before the first prompt

Mounting needs to `co_await`, and `init()` is not a coroutine. Spawning a mounter alongside the
shell would race the first prompt against the mount table. Awaiting it in `shell()` before the
prompt is the only ordering that is correct, and it is also where the "no OPFS" line belongs —
the third acceptance criterion is a sentence printed above the first prompt.

The work is in `boot_filesystem()` rather than inline so that its locals stay out of the
shell's frame, which has a size class to fit inside; `test_shell` guards that at 1 KiB and the
guard is why the split exists. It is idempotent, because a test boots the shell a dozen times
against one mount table and every one of those must not report the mounts as errors.

### Proving the reload without a browser

"Write a file, reload the page, the file is still there" reads like a criterion only a browser
can check. It is not: what a reload destroys is the `WebAssembly.Instance`, and what it
preserves is the store behind OPFS. `test/fakefs.mjs` puts the store in module scope, so the
smoke test writes a file, throws the instance away, instantiates the same module again, and
reads it back — with a real browser check still required before the milestone is believed, but
with the mechanism itself under CI.

The fake answers from inside the import, which no browser can do. The kernel cannot tell:
`wake()` only queues a resumption, and the tick that issued the request drains the queue on its
way out. One case in the smoke test holds replies back anyway and delivers them by hand, so the
genuinely-parked path is covered too rather than assumed.

### Smaller decisions

- **`Error::NotEmpty` is new.** `rm` on a populated directory needed a message of its own, and
  overloading `Invalid` would have printed "invalid" for the one error a user hits by accident.
- **`heap_new`/`heap_delete` are new.** `operator new` returns null on failure and
  `-fno-exceptions` means a plain `new` would then construct at address zero. Every allocation
  in `src/fs/` goes through the checked pair instead; `job.cpp`'s hand-rolled version is now one
  of them.
- **`vfs_list` folds in mount points.** A mount point need not exist in the filesystem beneath
  it, so `ls /` would not have shown `/home` at all. The table supplies the entry.
- **Listings are sorted in the VFS, not in `ls`.** Insertion sort over a directory, which needs
  no scratch and is small enough not to matter — and it means every consumer sees a stable
  order, not just the one that remembered to sort.
- **`persisted` is posted down from the page.** `navigator.storage.persist()` is not available
  in a worker (§A.2), and boot waits for the answer rather than guessing: `df` reporting the
  wrong durability is worse than a tick of delay.
- **`close` flushes.** A sync access handle buffers, and flushing on every write would give up
  the reason for using one. Closing is the point at which we know it is safe.

---

## M4 — Streams

Pipes with real backpressure, stdin, the whole shell grammar, and a `^C` that reaches a running
pipeline. This is the milestone where three debts recorded below come due at once — `send()`'s
policy, the owning token store quoting forces, and a shell that can watch the keyboard while a
child runs — and where the first thing in the system runs genuinely concurrently with another.
62,926 bytes of `kernel.wasm`, against an unchanged 256 KiB budget.

Nothing here changed a design *decision*, so `Concept.md` is not amended. Two comments that
described plans rather than facts are: `tty.h`'s, which proposed putting the screen behind a byte
channel, and `shell.cpp`'s, which named an argv-lifetime invariant that no longer exists.

### A pipe carries owning chunks, not `Bytes`

Milestones.md says `Channel<Bytes>` and `Bytes` already exists — `using Bytes = Span<const u8>` —
so the obvious reading is that a pipe moves `Bytes`. It cannot. A `Span` is a pointer and a
length, and the whole point of a pipe is that the reader runs *later*: by the time it takes the
value, the writer's buffer is a dead coroutine frame. The channel has to own what it carries, so
a pipe is `Channel<String, 8>` and `Stream::Write` copies into a `String` on the way in.

The copy is the price of the decoupling and it is the same copy `Bytes` would have needed
somewhere else, in a place with less obvious ownership. `String` also happens to satisfy exactly
what the ring needs — a default constructor for the inline slots and move-assignment for
`try_send` — which is not a coincidence: `Vec<String>` was already proven by the editor's history.

Capacity is counted in *chunks*, not bytes, so one huge write occupies one slot and backpressure
is per-write. Eight slots is enough to keep a producer and a consumer both busy without making
the block that holds them large; the number is one constant in `io.h`.

### What `send()` decided

M2 deferred blocking send because the decisions belonged to pipes. Here they are.

**A cancelled sender delivers nothing.** The value lives in the awaitable and dies with it; the
ring is only ever written by `put()`, which a cancelled sender never reaches. That is the only
answer that keeps cancellation meaning "unwinds by returning", because a half-delivered value
would have to be either dropped silently or delivered by a task that has already been killed.

**A full channel parks.** Not an error, not a drop — parking *is* backpressure, and a pipe that
dropped would make `ls | wc` report a number that depends on scheduling. The one place that
deliberately drops instead is the tty pump, for the reason M2 gives about the keyboard ring.

**Closing is directional, because a pipe is.** `close()` is the writer saying there is no more:
the receiver drains what is queued and only *then* reads `Err(Closed)`, so nothing in flight is
lost. `hangup()` is the reader saying it will take no more: parked and future senders get
`Err(Closed)`. Two verbs rather than one, because "the far end is gone" means opposite things at
the two ends, and a single `close()` would have had to guess which.

`Error::Closed` is a new value rather than a reuse of `Again`. `Again` is already the stray-wake
sentinel — "you were woken but there is nothing here" — and end of input is the opposite claim.
Overloading it would have made a spurious `wake()` from JS indistinguishable from EOF.

### `take()` wakes the sender, not `await_resume`

The obvious symmetry to `try_send` waking a parked receiver is `Recv::await_resume` waking a
parked sender, and it deadlocks. `Recv::await_ready` is `!empty()`, so a receiver that finds the
ring non-empty never suspends and is never resumed by a wake — and that is the *ordinary* case
for a pipe, where the reader is usually behind. The wake has to live in `take()`, which is the
one thing every path that removes a value goes through.

The same reasoning put the closed check into `await_ready`: a receiver parked on an empty pipe has
nothing coming to wake it when the writer closes, so `close()` has to wake it explicitly and
`await_ready` has to admit that a closed empty channel is ready.

### The intrusive waiter queue is still not needed, and now says so

M2's notes hand this milestone the job of putting intrusive queue links inside `Waiter`, so that
deregistration lives in one place, "when `send()` needs it too". It does not need it. Every
channel in the system has exactly one writer — each pipe has one upstream stage, `err` is the
console, the input pipe has only the pump, and the report channel uses `try_send` — so a single
`send_token_`, symmetric with the existing `recv_token_`, holds every case, and the existing
cancellation path works untouched.

That is true of *this* grammar. `2>&1` would join two writers onto one channel and silently
clobber a token, which is the kind of bug that shows up as a lost wakeup three milestones later.
So `Send::await_suspend` and `park_sender` panic if a second sender arrives, and the operator that
would trip it arrives with the queue work rather than before it.

### The pipeline is a heap block, and could not be anything else

The natural shape is locals in the shell's frame: the parsed pipeline, the pipes, the report
channel. Two things forbid it, and both are load-bearing enough that `test_shell` guards them.

The allocator's top size class is 512 bytes and anything above it takes a whole 64 KiB span, so a
shell frame carrying a pipeline would cost a span *per shell*. And `~Sched` destroys jobs in spawn
order — the shell first — so a stage frame pointing into the shell's frame is a use-after-free
during `sched_reset`, which the test suite does after every case.

One object answers both: a refcounted heap block holding the frozen pipeline, the pipes, the
report channel and the pid list. The shell holds a reference, each stage holds one through an RAII
local in its own frame, and the last one out frees it — so the order they leave in stops
mattering. `~Sched` also now runs backwards, since a child is spawned after its parent, and
`sched_cancel` stands down while it does, because `jobs` holds freed pointers as that loop walks.

The pipes inside it are individually heap-allocated rather than an array, for a smaller reason
with the same shape: `Channel` deletes its copy constructor, which suppresses the implicit move,
so `Vec<Channel>` does not compile — and an inline array of eight would have pushed the block past
512 anyway.

### A tty pump, not a `select`

M3's notes name the two candidates for interrupting a running program: a second receiver on a
single-receiver channel, or a `select`-shaped combinator. Neither is what landed, because both
fight the same constraint from opposite sides — `CancelState::waiting` is a single slot, so one
task tree can be parked on exactly one awaiter, and a select over two channels needs two.

Instead the keyboard changes hands. While a pipeline runs, a spawned pump coroutine is the only
receiver on `keys()` and the shell is parked on the job's report channel; when the job ends the
pump is cancelled and the shell takes the keyboard back. The one-receiver rule holds at every
instant, and it holds *structurally* rather than by luck: `sched_unwait` knows nothing about
channels, so a cancelled pump leaves its token in `keys()` until its `~Recv` runs, and the shell
must not re-register before then. That is why the pump files a report of its own and the shell
waits for it — the report is proof the pump has unwound. The equality guard in `~Recv` is now
load-bearing for two receivers rather than one, and is commented as such.

The pump never parks on the input pipe, and that is not an optimisation. `ls | grep foo` reads no
stdin, so a blocking pump would fill the eight-slot ring after eight keystrokes, park, stop being
the receiver on `keys()` — and make `^C` unreachable, defeating the milestone's own criterion
within a second of key autorepeat. It drops instead, which is the policy `key()` already uses on
the keyboard ring and for the same reason: there is nowhere to report a full ring to.

This also means the shell has exactly one path. A single command is a one-stage pipeline with a
pump, costing two extra job records and two frames, because the `^C` criterion is met by
`sleep 5000` and a fast path for single commands would have left precisely that case broken.

### Structured concurrency, put back by hand

§3.6 says a parent `co_await`s a child group and cancellation propagates down the tree. The stages
here are independent scheduler jobs with independent `CancelState`s, so it does not. That is
forced rather than chosen: the stages of a pipeline must all be parked at once, one job cannot
have two children parked at once, and `sched_spawn` is the only concurrency there is.

What §3.6 buys is put back explicitly. A destructor in `run_line`'s frame cancels every launched
stage and the pump — a destructor rather than a branch, because a cancelled coroutine cannot park
again to clean up (every `await_suspend` here declines to suspend once the flag is set) and
because the frame may be destroyed outright rather than resumed. The same reasoning shapes a
stage's epilogue: closing its output, hanging up its input and filing its report all happen in a
destructor, so they also happen when the stage is cancelled or destroyed while suspended.

That epilogue is the whole of the early-close mechanism. `head -n 1` needs no way to say "I am
done" — it returns, its runner hangs up its input, and the next write upstream gets `Err(Closed)`.

A real child-group awaitable is what would retire this, and it needs the intrusive `Waiter` links
above. The two deferrals are the same deferral.

### A frame that will not allocate is now a value

`operator new` returns null and its comment says callers must check, but no promise declared
`get_return_object_on_allocation_failure`, so the compiler was entitled to assume it never does.
`sched_spawn`'s `if (!t) return 0` was written in anticipation of this and could not fire.

M4 is where it matters, because "how many stages actually launched" is a number the shell has to
wait for exactly. So `TaskPromise` declares the handler, which also switches frame allocation to
the nothrow `operator new` the standard names for it — hence a `std::nothrow_t` in `types.h`,
which is a declaration the freestanding build has to supply like everything else. Awaiting a task
whose frame never allocated panics rather than reading a promise that does not exist; only a
spawned root turns the failure into a value, which is the only place that can do anything with it.

### The terminal stays a cell grid

`tty.h` said M4 would replace the console sink with a pair of `Channel<Bytes>` and move the screen
behind a reader task. It does not, and the comment is amended instead. §2.3 is the stronger
argument: the terminal *is* a cell grid, and a byte channel in front of it would add a copy, a
frame and a scheduling hop to reach an array `screen_write` already fills synchronously — while
putting terminal output behind a bounded queue that can drop. The one thing the reader task would
have bought, `prog | prog` and `prog | screen` sharing one mechanism, the `Stream` function
pointer gives for nothing.

`Stream` did have to grow. A sink can now answer `Err(Again)` for "full, park me" and gets a
second function pointer to arm the wake token with; `Write` gained a `Waiter` and, more
importantly, a destructor that deregisters it, without which a frame destroyed mid-write leaves a
dangling pointer in the wake table — exactly the bug the channel notes below describe. `Write`
retries once after being woken, which is enough because a pipe has one writer and being woken by
`take()` means there is room; a second `Again` is a stray wake, and `write_all` loops for the
programs that care. `Stream::write` stayed an awaitable rather than becoming a `Task` for the
reason M3 gives: a coroutine frame per write is the hot path §8.2 warns about.

`Source` is its mirror, and `Stdio` gained an `in`. A program the shell gave no input gets a
source that reports EOF immediately rather than a null one, so no program has to check.

### The store that replaced the borrow

M3's notes predicted this exactly: quote removal produces words that are not substrings of the
line, which destroys the zero-copy argv path and forces an owning token store the parser is built
around. Both halves happened.

Words are appended to one growing `String` and turned into `Str` only by `freeze()`, because the
store reallocates while it grows and every view into it would move. Making that a rule enforced by
the API rather than by a comment matters more than it looks: `add_word` panics after `freeze`, and
the accessors panic before it, so the one ordering that must hold cannot be got wrong quietly.
Moving a *frozen* pipeline is still safe — `String` and `Vec` move by stealing the pointer, so
nothing shifts — and that is what lets the parse result live in the job block rather than in a
frame. It would not be true of a small-string-optimised string, which is worth knowing before
anyone adds one.

`Args` stays a non-owning `Span<const Str>`, now over the store rather than over the shell's line
buffer. The invariant M3's comment called "the single easiest thing for M4's pipelines to break"
is not preserved so much as retired: nothing borrows from `line.text` any more.

Redirection targets live in a second table rather than among the words, so that a command's argv
stays contiguous. `> f ls` would otherwise put `f` in `ls`'s argv.

### Redirection is parsed and refused

Quoting, escaping, `|` and `>` are one grammar and writing half of it means writing it twice, so
all of it is written. There is no filesystem to redirect *to* until M5, so a path target is
refused — but at pipeline setup, before anything is spawned, rather than at the first write. A
sink that fails on its first write would let a command run and produce its side effects before its
redirection turned out to be impossible, which is not what a shell does. When M5 lands, one
function turns the refusal into an open.

`out` and `err` are still the same sink, for the reason M3 gave: `2>` is parsed, so the split is
now expressible, but there is nowhere for it to go.

### `ls` lists the registry

The criterion is `ls | grep foo`, and `ls` lists a filesystem that arrives a milestone later.
Rather than reword the criterion or pull `MemFs` forward, `ls` lists the program registry — which
is what `/bin` will hold, and which §4's tier table already calls a kernel applet's job. The
criterion is met literally, the pipeline it exists to prove is proved, and M5 replaces a loop over
`program_first()` with a walk of the mount table while the test stays green.

### Smaller decisions

`clear` still writes to the grid rather than to `io.out`. Clearing is a grid operation with no
byte representation, so `clear > f` is meaningless; the honest cost is that `clear | cat` clears
the screen, which is what it says it does.

`grep` is a substring match, and its usage line says "text" rather than implying a pattern
language. `cat` copies chunks rather than lines, so it is byte-exact and a final line without a
newline stays that way; `grep`, `head`, `tail` and the rest go through a `LineReader` that keeps a
partial chunk, so a line may span any number of chunks. `wc` counts over raw chunks for the same
reason — nothing should depend on where a chunk happens to break.

`LineEnd::Eof` is still not there. ^D closes a program's stdin through the pump, which is where
end of input actually means something; at the prompt there is nowhere to exit to, and inventing a
meaning for it before there is one is how enums acquire dead members.

The size more than doubled, 28,282 to 62,926 bytes, which is 24% of the budget M3 raised with
exactly this in mind. Six new programs, a lexer, a parser, the job runtime and a second awaitable
pair account for it; the budget is not moved.

---

## M3 — Userland shell

The `LineEditor` coroutine §3.5 promised, a tokeniser, the self-registering program registry of
§3.6, argv and exit codes — and the first build where the thing on screen is an operating
environment rather than a demonstration. 28,282 bytes of `kernel.wasm`, against a budget raised
from 32 KiB to 256 KiB in this milestone.

### Static initialisers now run, and one invariant is retired

M0 left a question open — "self-registration needs `__wasm_call_ctors`, which `--no-entry` leaves
uncalled, and that question is better settled in M3 where the program registry actually depends
on it" — and this is where it is settled. `init()` calls `__wasm_call_ctors()` itself.

The alternative was a linker-section table: a `constexpr` descriptor per program placed in a
custom data section with `__attribute__((section, used, retain))`, walked between the
linker-defined `__start_`/`__stop_` symbols. It needs no constructors at all, so it would have
kept the invariant intact. It was not chosen because §3.6 says "populated at static-init time by
an inline registrar" and there was no reason to route around the spec; because the section trick
is a second, undocumented dependency on linker behaviour on top of the ones Appendix C already
records; and because static init is a capability the whole system wants once, not a trick one
subsystem uses.

`__wasm_call_ctors` is synthesised by wasm-ld with hidden visibility, so a plain `extern "C"`
declaration reaches it and **no export is added** — the exact-surface assertion in `run.mjs` is
the guard on that claim, and it would have fired on the first build if the assumption were
wrong. The call sits *after* `heap_init`, so a constructor added later may allocate; that
ordering is the new invariant and it is commented at the call site.

What does not change is the destructor half. `__cxa_atexit` is still unprovided, deliberately, so
a namespace-scope global with a non-trivial destructor is still a link error. `Heap`, `Screen`,
`Channel` and the registry's list head remain PODs; `Sched` remains behind a pointer. CLAUDE.md's
statement of the rule has been amended, because its first clause — that `--no-entry` never calls
`__wasm_call_ctors` — is now false of this kernel.

`tests.wasm` calls it too, so the cases see the registry the shipping kernel sees. Its own case
list stays explicit in `main.cpp`: the order is load-bearing where cases share global state, and
converting it would be an unrelated refactor riding along in this milestone.

### A sorted intrusive list, not a `HashMap`

M1's notes anticipated the registry wanting `HashMap`'s FNV-1a overload for `Str` keys. It does
not. `help` has to *enumerate* the registry and `HashMap` has no iteration API; `HashMap::insert`
allocates, and a static-init registrar must not touch the heap before anyone has reasoned about
whether it exists; and with seven programs a linear scan of `Str` compares is not measurable
against the coroutine frame the lookup is about to allocate anyway.

Insertion is sorted rather than push-front, which costs nothing at seven entries and buys
something specific: the order of static initialisation across translation units is unspecified,
so a push-front list would make `help`'s output depend on the link order, and therefore make it
untestable. Sorted, `help` needs no sort of its own and the smoke test can assert the listing.

### `src/prog/` is an OBJECT library, and that is not a detail

Nothing in the system references `src/prog/echo.cpp` by name. Those translation units reach the
link only through their registrars, and `--gc-sections` never extracts an archive member that no
symbol references — the same trap `CMakeLists.txt` already documents for `main.cpp`. As a
`STATIC` library, `src/prog/` would link cleanly and produce a kernel with an empty registry: no
warning, no error, a shell where every command is "not found". CMake puts an OBJECT library's
objects directly on the consuming link line, which is exactly the property required.

Because that failure is silent, `test_prog` asserts the exact *count* of registered programs and
their order, not that a few known names are present. A spot check would survive losing the
programs it does not name.

### The exit code goes in the prompt

"A nonzero exit code is observable" has two obvious readings: a diagnostic line after every
failure, or a status indicator in the prompt. The prompt wins on four counts. It is one screen
read for the smoke test — `false` followed by a row reading `[1] $` proves the criterion in a
single assertion, and `nosuch` followed by `[127] $` proves the not-found path in the same shape.
It invents no stream semantics before M4 defines them: a diagnostic line for every nonzero status
would be the shell writing to a stderr that does not yet mean anything, and no real shell does
it. It costs nothing in the common case. And it composes forward, since a pipeline's status is
its last command's and nothing about the prompt changes.

The shell reads that status by `co_await`ing the program's `Task<i32>` rather than spawning it.
That is not a style choice: `sched_tick` reaps a finished job and destroys it, so the promise's
`i32` is unreachable after the fact, and awaiting is the only way to see it at all. Awaiting is
also what propagates the `CancelState` into the program, and what makes the single-receiver rule
on the keyboard hold — while a program runs the shell is suspended inside `co_await`, not on
`keys()`, so nothing can displace anything. Keys typed during a program stay in the ring as
typeahead.

### `Vec<char32_t>` for the line, and a redraw that infers its own scrolling

The line buffer is one codepoint per element, not UTF-8. In M3 one codepoint is one cell, so
every editing operation and all of the redraw's column arithmetic is plain indexing; with a
`String`, Left, Right, Backspace, kill-word and the wrap calculation would each need a codepoint
scan, and mid-line insertion would need a byte shuffle regardless. The cost is four bytes per
character against a span allocator, which is noise, and the single UTF-8 encode happens once, at
Return. The payoff is that `String::insert`/`erase` never had to be written; `Vec::insert`/`erase`
did, and they are useful to everything else.

The screen has no erase-to-end-of-line, no insert-character and no scroll counter, so the editor
repaints the whole line from an anchor on every keystroke and blanks the tail by hand, tracking
how many cells the previous paint covered. The interesting part is keeping the anchor correct
when a paint scrolls the grid: nothing reports a scroll, so the editor computes where the write
*should* have ended — `y0 + (x0 + n - 1) / cols`, using the deferred wrap — and takes the
shortfall against the actual `cursor_y` as the number of rows the grid moved. That is exact,
because `screen_newline` is the only thing besides our own writes that can move the cursor.

Two consequences worth stating. `screen_move` clamps to `cols - 1`, so the deferred-wrap column
is unreachable by cursor addressing; the editor places a cursor at an exact multiple of `cols` at
column 0 of the *next* row, which is not a compromise — after `wrap_pending` that is genuinely
where the next character lands. And a line longer than the whole grid pushes its own prompt off
the top, after which the anchor clamps at row 0 and the leftmost cells are wrong. Fixing that
needs a line model the grid does not have, which is the M7 layout layer's; M3 accepts the
cosmetic glitch and tests the case that matters, where the anchor follows a scroll correctly.

An unconditional repaint is more work than the common case needs — appending at the end with no
wrap is one `screen_put`. It is a few hundred cell writes coalesced into one damage rectangle and
one `host_present` per tick, which is nothing at keyboard rates, and the optimisation can be
added later against a test suite that already pins the behaviour down.

### `Stream::Write` does its work in `await_suspend`

§3.6 fixes the program signature as `Task<int>(Args, Stdio)`, and M4 will put a `Channel<Bytes>`
behind `Stdio` where a write to a full pipe has to park. Writing `io.out.write(s)` as a plain
call now would mean rewriting every call site in `src/prog/` then; writing it as `co_await
io.out.write(s)` from the start costs a suspend point that is never taken.

The work happens in `await_suspend`, which returns `false` to resume immediately, rather than in
an `await_ready` that returns `true`. Only `await_suspend` receives the coroutine handle, and
therefore the promise, and therefore the `CancelState` — an awaitable that completes in
`await_ready` would be the one thing in the system that cannot see cancellation, which §8.1
exists to forbid. A never-taken suspend point in exchange for the rule holding everywhere is a
good trade.

`Stream` is a function pointer plus a `void *`, not a virtual interface. There will be exactly
two implementations, and a vtable costs a data section and an indirect-call table entry per
implementation for no gain. `out` and `err` are the same sink in M3, because the split is
meaningless until there is redirection to tell them apart.

### The tokeniser has no quoting, on purpose

Quote *removal* produces tokens that are not substrings of the input, which destroys the
zero-copy property the whole argv path depends on — `Args` is a `Span<const Str>` over views into
the shell's line buffer, and nothing copies. Supporting quotes would force an owning token store
that M4's parser would then have to be built around. And quoting, escaping, `|` and `>` are one
grammar: writing half of it now means writing it twice. The visible consequence is that `echo 'a
b'` prints the quotes, which is stated in `echo`'s usage line rather than hidden.

The lifetime that makes this work — `argv` borrowing from `line.text`, which is a named local in
the shell's frame and stays alive across the `co_await` of the program — is commented in
`shell.cpp`, because it is the single easiest thing for M4's pipelines to break.

### What ^C does, and what it does not do yet

Typed at the prompt, ^C writes `^C`, abandons the buffer and returns `LineEnd::Interrupt`; the
shell prints a fresh prompt carrying 130. Typed while a program runs, it sits in the keyboard
ring and is consumed as typeahead by the next `read_line`.

Interrupting a *running* program is Milestones.md's M4 criterion and stays there. It needs the
shell to watch the keyboard while a child runs — a second receiver on a single-receiver channel,
or a `select`-shaped combinator — and both are streams work. What M3 owes is that the mechanism
underneath is already in place, which is what the cancellation cases in `test_edit` and
`test_shell` assert: `sched_cancel` on the shell unwinds through `co_await`ing a program, through
a running `sleep`, and out of a `Recv` parked on the keyboard, with the channel left usable.

`LineEnd` is a named enum rather than a bool for the same reason: M4 and M7 will add `Eof` and
whatever a job-control shell needs, and the signature should not change when they do.

### Smaller decisions

`sleep` takes **milliseconds**. There is no float parser, the scheduler is a millisecond machine,
and the smoke test needs an exact number to assert `tick()`'s return value against. The
divergence from POSIX lives in the usage string.

`read_line` is `Task<Result<Line>>` on the `LineEditor`, where §3.3 sketches `Task<Line>
read_line(Tty&)`. §3.3's sketch already diverges from shipped signatures — it lists
`Task<void> sleep_ms(u32)` where the kernel has `Task<Result<void>>` — so it is read as
illustrative, and `Concept.md` is not amended. Nothing in M3 changed a design *decision*, which
is the bar for touching the spec.

`utf8_decode` moved out of `screen.cpp` into `src/kernel/text.h`, because the editor needs to
decode history entries and two decoders in one system is one too many. The behaviour changed in
one untested corner: a stray continuation byte now yields U+FFFD and draws, where it used to be
skipped silently. Visible corruption beats invisible corruption.

Ctrl-W is bound to kill-word and unit-tested, but `web/keys.js` deliberately leaves Ctrl+W to the
browser, which closes the tab — a page that swallows it is a page you cannot leave. So
Alt-Backspace is bound to the same action and is the one that actually reaches a browser. That is
a keybinding decision, not a change to what `keys.js` forwards.

### The budget moved

M0 set 32 KiB and M1 and M2 stayed well inside it. M3 does not: the shell, the editor and seven
programs took `kernel.wasm` from 14,011 to 28,282 bytes, about 86% of the old ceiling, with M4's
streams and M5's filesystem still to come. The budget is now 256 KiB. That is a deliberate act,
as the file's own comment requires, and the reasoning is that 32 KiB was a nucleus-sized number
chosen when the nucleus was all there was; a self-contained operating environment with a
filesystem and a program set is not a 32 KiB artifact, and a ceiling that has to be raised every
milestone measures nothing. 256 KiB is still small enough that a regression of the kind the check
exists to catch — a libc dependency, an accidental template explosion — moves it visibly.

Roughly 4.4 KiB of the 28 KiB is the wasm `name` section. It is kept: `--strip-all` would remove
it, and with it every symbol name in a browser stack trace.

---

## M2 — Screen and keys

The cell grid, its damage rectangle and the canvas renderer, `Channel<T>`, and the `key` and
`resize` exports that complete §3.4's five — §2.3 and §3.5 made real, plus the first code in the
system that a user can see. 14,011 bytes of `kernel.wasm`, against the same 32 KiB budget.

### `resize` returns where the screen is

§3.4 lists `resize(cols, rows)` with no return value, but the renderer has to learn three things
from somewhere: the address of the cell array, the geometry, and where the cursor is. Four
mechanisms could carry them. A hard-coded address reverses M0's deliberate decision that the host
stays ignorant of the kernel's memory map. Exporting a wasm global needs a linker flag, and
exports are named with `BRAAM_EXPORT` or not at all. Widening `host_present` re-sends unchanging
geometry on every frame and only tells the host anything *after* the first paint. A separate
`screen()` export is the honest alternative and was close, but `resize` is already the one call
that reallocates the grid, so it is already the moment every cached view has to be re-derived
(§8.4) — making it also the moment the address is handed over keeps that discipline in one place
instead of two, and keeps the export list at the five §3.4 names.

So `resize` returns the address of a static `Screen` descriptor, or 0 if the new grid could not be
allocated. Static, not heap, so the address is a link-time constant the host can hold forever;
and it carries a `'BSCR'` magic word, so a renderer paired with the wrong build says so rather
than drawing noise. §3.4 is amended.

Failure is all-or-nothing: the replacement grid is allocated and filled before anything is
published, so a `resize` that returns 0 leaves the old screen whole and still on display. And the
geometry is clamped — 512 columns by 256 rows — because `cols * rows * sizeof(Cell)` is computed
in a 32-bit `usize`, and a host that asked for 30000×20000 would otherwise wrap it to a small
allocation and then write past the end. The host reads `cols` and `rows` back out of the
descriptor instead of assuming it got what it asked for, which makes clamping, out-of-memory and
success one path on the JS side: *draw what the descriptor says*.

### One rectangle, flushed once a tick, with the cursor folded in

Damage could be presented per write, which would mean an import call per character. It is instead
accumulated into a single rectangle and flushed from `tick()` after `sched_tick()` returns, so a
tick that typed a line presents once and an idle tick presents not at all. `tick()` in `main.cpp`
does the flushing rather than the scheduler, so the screen does not become a dependency of the
scheduler.

The cursor is drawn by the renderer and stored nowhere, which means moving it dirties two cells:
the one it left and the one it entered. Marking both at every site that moves the cursor works
until someone adds a site and forgets — and M3's line editor will add several. So `screen_flush`
remembers where the cursor was last drawn and folds the move into the rectangle itself. Mutations
now only have to mark cells they actually wrote, and the ghost-cursor bug is unavailable by
construction.

### Channel wakeups reuse the token table

A receiver suspended on an empty channel has to be resumed by whichever `try_send` fills it. The
obvious mechanism is a new scheduler entry point taking the `Waiter *` the channel holds — and it
is a trap. `sched_cancel` unwaits and readies a waiter, but `sched_unwait` knows only about the
timer queue and the wake table; it cannot unlink from a channel it has never heard of. A cancelled
receiver would therefore sit on the ready queue while still listed in the channel, and the next
`try_send` would queue the same handle a second time. Fixing that properly means intrusive queue
links inside `Waiter` so that deregistration stays in one place, which is real machinery and, on
the evidence, M4's to build when `send()` needs it too.

The channel instead allocates a wake token and stores only the token, not the pointer.
`sched_wake` on a token nothing waits on is already defined to be ignored — "a late or cancelled
event" — so a stale token after a cancellation is ordinary traffic rather than a use-after-free,
and every existing path works untouched: `sched_unwait` in the awaiter's destructor deregisters,
`sched_cancel` already knows how to pull a token waiter out. Nothing in `sched.h` or `sched.cpp`
changed for M2.

The cost is a hash insert and remove per suspension, which is nothing at keyboard rates and worth
revisiting in M4 when `Channel<Bytes>` carries pipe traffic. The price of a globally visible token
is that a stray `wake()` from JS can resume a receiver spuriously, so `await_resume` checks the
ring rather than trusting the wake and returns `Error::Again` when there is nothing to take.
Without that check the count would underflow.

### `Channel<T>` gets its mechanism, M4 gets its policy

§3.6 specifies both `co_await ch.recv()` and `co_await ch.send(v)`. Only `recv` and a
non-blocking `try_send` landed. The size argument for deferring would be bogus — an uninstantiated
member of a class template emits nothing — but blocking send needs decisions M2 has no way to
make: what a cancelled sender does with its half-delivered value, whether a full channel with no
receiver parks or errors, and what closing one does to the senders waiting on it. Those are pipe
semantics, and M4 defines them. An awaiter nothing awaits is an awaiter nothing tests, and the
test suite is the thing that has found every real bug so far.

The ring is inline rather than heap-allocated, which is what lets the keyboard channel be a plain
global. A `--no-entry` binary never runs `__wasm_call_ctors`, so a global has to be correct when
zero-initialised and trivially destructible — the same constraint that pushed the scheduler behind
a pointer in M1, solved the other way here because a fixed-capacity ring has no allocation to do.
Sending therefore cannot fail for want of memory, which matters because `key()` is called from the
host with nowhere to report an error to. A full ring drops the newest event, which is the right
failure for a keyboard.

### Keys are codepoints; there are no control characters

`key(code, mods)` carries a Unicode codepoint for anything printable and a value above `0x110000`
for the named keys, so the two can never collide. Enter, Tab and Backspace are named keys, not
`0x0D`, `0x09` and `0x08` — the temptation to encode them as control characters is exactly what
§2.3 exists to refuse. `^C` arrives as `'c'` with the control modifier set and means whatever its
reader decides; there is no byte anywhere in the system that has to be recognised as an
interrupt, and nothing to mis-parse.

`key()` only queues. Like `wake()`, it never resumes a coroutine, so an event arriving from the
host cannot re-enter the scheduler — and because the worker is single-threaded and `tick()` is a
synchronous call, it cannot arrive mid-tick anyway. The rule is kept for the same reason `wake()`
keeps it: it makes the question moot for every event source added later. The corresponding
obligation on the host is that `key()` and `resize()` are each followed by `pump()`, since an idle
kernel has no timer armed and queued work would otherwise wait forever.

### Reflow keeps the rows in use, not the bottom of the grid

"Window resize reflows" has three plausible readings. Full re-wrapping of logical lines is the one
a modern terminal does, and it needs a per-row continuation bit and a notion of line length that
the grid does not have; that is properly M7's, where the layout layer decides who owns line
structure. Keeping the top-left corner is not a reflow but a crop, and it discards precisely the
recent output the user is looking at.

Keeping the bottom-most rows is the obvious remaining answer, and it is wrong in the common case:
a 24-row screen holding one line of output has its text at the top, so keeping the bottom five
rows of it keeps five blank rows and throws the text away. The smoke test caught exactly that on
the boot banner. What survives is the rows *in use* — `0..cursor_y` — dropping from the top when
they no longer fit and landing at the top of the new grid, since output grows downwards and that
is where the eye already is. A full screen still keeps its bottom, because there the rows in use
are the whole grid.

The wrap is deferred for the same reason: the cursor parks at `cursor_x == cols` after the last
column is filled and only descends when the next character arrives. Wrapping eagerly would scroll
the screen the moment a line reached the right edge, before there was anything to put on the next
one.

### The renderer, and where the font lives

Rendering is the ~300 lines of JavaScript §2.3 promises, in `web/render.js`, and it does exactly
one thing: read cells, draw glyphs. It never calls back into the kernel — `host_present` runs
synchronously inside `tick()`, so a call the other way would re-enter the scheduler mid-drain.
That rule is written next to the import.

The split between page and worker follows what each one can know. The page owns the pixel box and
reports it in device pixels via `ResizeObserver`'s `devicePixelContentBoxSize`, which is already
correct under fractional zoom; it also has to watch `devicePixelRatio` with a `matchMedia` query
re-armed at each new ratio, because moving a window to another monitor changes it and nothing else
reports that. The worker owns the font, so it owns the metrics and therefore the geometry: it
measures a glyph, divides, and calls `resize`. `devicePixelRatio` does not exist in a worker at
all, which settles the question of who computes what. Advance widths are fractional, so the cell
width is rounded once and every glyph is placed at `col * cellW` rather than letting the font
advance across a row; a startup check compares `M` against `i` and warns if the font turned out
not to be monospaced, because that failure otherwise looks like a kernel bug.

There is no blinking cursor. A blink needs a timer, `tick()` would then never return `-1`, and the
page would never go idle — a visual flourish is not worth a kernel that never sleeps.
`transferControlToOffscreen` has no fallback: without `SharedArrayBuffer` the main thread cannot
see the kernel's memory, so main-thread rendering is not available at any price, and its absence
is reported rather than worked around.

### What the smoke test now proves

`init` creates an 80×24 grid before anything else, so the kernel is never in a screenless state
and the boot banner has somewhere to go. The host's first `resize` then reflows that banner into
the measured geometry, which makes the reflow path visible on every page load rather than only
when someone drags a window.

Both M2 criteria are checked against the shipping `kernel.wasm`, not only against `tests.wasm`:
the smoke test resizes, types through `key()`, and asserts the codepoints landed in the right
cells, that the cursor advanced, and that exactly one `host_present` arrived covering the two
written cells *and* the cell the cursor left. Then it shrinks the screen and asserts the text
survived with the cursor still inside the grid, and that an absurd geometry comes back clamped.
The M1 assertions are unchanged and still pass: the console task suspends on a channel rather than
a timer, so it cannot perturb the tick delays the M1 test pins down.

---

## M1 — Scheduler

`Task<T>`, a ready queue, a timer queue, wake tokens, `tick()`, `wake()`, `sleep_ms` and
cancellation — the kernel core §3.3 describes, plus the `HashMap` and `String` that M0 deferred.
8,625 bytes of `kernel.wasm`, against the same 32 KiB budget.

### Timers belong to the kernel, not the host

§3.4 listed both a `host_timer(token, ms)` import and a `tick(now_ms)` that "returns
ms-until-next-timer, or -1". Those overlap: the second only means anything if the kernel knows
when its next deadline is, and if it knows that, the first is redundant. Only one of them can be
the design.

The kernel keeps the timer queue. `sleep_ms` inserts a deadline, `tick` fires whatever has come
due and reports the delay to the next one, and the host's entire timing responsibility is
`setTimeout(pump, delay)`. This wins on three counts. There is one host timer outstanding
instead of one per sleeping task. The import surface stays at two, so the smoke test's
assertion that nothing new appeared is still a meaningful statement about libc. And, most
usefully, the clock is a *parameter*: tests call `tick(0)`, `tick(10)`, `tick(15)` and assert
exact wake ordering with no real time involved and nothing to flake. Both M1 acceptance criteria
are checked that way, in `tests.wasm` and again against the real `kernel.wasm` in the smoke test.

§3.4 is amended to say so. The rounding in `tick`'s return is deliberately upward, so the host
never wakes before a deadline and re-arms for the remaining fraction of a millisecond.

### Cancellation rides in the promise

§8.1 asks that `CancelToken` participate in every awaitable from this milestone on. The obvious
reading is a parameter — `sleep_ms(500, token)` — but a rule enforced by remembering to pass an
argument is not enforced at all, and it puts the token in every signature in the system.

Instead the promise carries a `CancelState *`, and every awaiter's `await_suspend` is templated
on the promise type:

```cpp
template <class P> bool await_suspend(std::coroutine_handle<P> h) {
    w_.cancel = h.promise().cancel;
```

The compiler hands `await_suspend` a `coroutine_handle<promise_type>`, so an awaiter can reach
the *awaiting* coroutine's state without being told about it. `Task`'s own awaiter copies the
pointer from parent to child, which is where §3.6's "cancellation propagates down the tree" comes
from: it is one assignment, made structurally, rather than a tree walk. The cost is that every
awaitable in the kernel must be awaited from a `Task` — acceptable, since that is what a process
is.

Killing sets the flag and, if the tree is suspended, pulls its waiter out of the timer queue or
wake table and puts it back on the ready queue. It then resumes normally, sees the flag, and
returns `Err(Error::Cancelled)`. Nothing is destroyed from outside: the coroutine unwinds by
returning, exactly as §3.6 requires, and its destructors run on the ordinary path. A task that
is on the ready queue rather than suspended needs no special handling — its next `await_suspend`
sees the flag and declines to suspend.

That propagation only works if errors actually propagate, and here M0 had left a trap: `TRY`
expands to a plain `return`, which is ill-formed inside a coroutine. `CO_TRY` and `CO_TRY_VOID`
are the same macros with `co_return`, and they live beside `TRY` so the trap and its fix are read
together. A process root is different — it converts the error to an exit code rather than
propagating it — so the demo and the test tasks check the `Result` explicitly instead.

### The waiter lives in the coroutine frame

§3.3 describes the suspended-task table as `HashMap<u32, coroutine_handle<>>`. What is stored is
a `Waiter *` instead: a small record holding the handle, the cancel state, the token, and room
for the payload that `wake(token, ptr, len)` already promises to deliver.

The record lives *inside* the suspended coroutine's frame — it is a member of the awaiter, which
the language guarantees stays alive across the suspension. So registering a wait allocates
nothing, and `wake()` has somewhere to put a payload that the awaiter can read on resume without
a second lookup. Nothing about the table's shape changes; it just has a value type with more
than one field in it.

The cost of a pointer into a frame is that destroying the frame must not leave it behind, so
every awaiter deregisters in its destructor. That is the one rule this design has to get right,
and it is what makes `sched_reset()` — and, later, killing a process mid-await — safe rather than
a use-after-free.

`wake()` only queues. It never resumes a coroutine, so an event arriving from JS in the middle of
a `tick()` cannot re-enter the scheduler. An unknown token is ignored rather than an error: a
wake arriving after its task was cancelled is normal traffic, not a fault.

### Scheduler state is allocated, not static

A `Vec` or `HashMap` at namespace scope has a non-trivial destructor, and clang registers those
with `__cxa_atexit` from the static-init function — which `--no-entry` never calls, but which
still references a symbol nothing provides. Under M0's deliberate removal of `--allow-undefined`
that is a link error, and rightly so.

So the scheduler's state is one struct behind a pointer, built on first use. The global is a
plain pointer, there is no static initialisation to worry about, and the reset that unit tests
need between cases falls out for free: destroy the struct and drop the pointer. Its destructor
runs jobs down first, so suspended frames are destroyed while the queues they point into are
still alive.

### Queues sized for the actual workload

The ready queue is a `Vec` with a head cursor rather than a deque: it is drained to empty on
every tick, so the cursor never travels far and the storage is reused rather than reallocated.

The timer queue is a `Vec` sorted with the earliest deadline last, so firing pops from the back
in O(1) and inserting is a bubble through a list that is a handful of entries long in any real
workload. A binary heap would improve the insert and make the removal worse — and removal by
waiter is exactly what cancellation needs, which is a linear scan in a heap too.

Both are honest bets on scale rather than defaults, and both are contained: the ready queue and
timer queue are private to `sched.cpp` and can be replaced without touching an awaitable.

### `HashMap`, shaped by the wake table

Open addressing with linear probing, power-of-two capacity, tombstones, doubling at three
quarters full. Integer keys go through murmur3's finalizer, because sequential wake tokens are
the primary key type and the identity hash would turn the table into a single long probe run.
There is an FNV-1a overload for `Str` keys, which the M3 program registry will want.

Slots are one array of `{key, value, state}` rather than parallel arrays. The kernel's tables are
small and looked up one key at a time, so the cache argument for splitting them does not apply,
and one array is half the allocation bookkeeping. Insertion returns `false` on OOM in the same
style as `Vec`.

### `sleep_ms` is a `Task`, and that costs a frame

The awaitable underneath `sleep_ms` is enough on its own — `co_await Sleep(500)` would work and
allocate nothing. It is still wrapped in a `Task<Result<void>>`, because §3.3's "every syscall is
one of these" is worth more than one allocation: syscalls compose, cancel and propagate errors
uniformly precisely because they are all the same type. §8.2 says the allocator is built for
coroutine frames as its primary workload, so this is spending exactly what that was built to
spend.

### The demo, and what the smoke test now proves

`init` spawns two tasks that sleep past each other — a at 10 ms and 30 ms, b at 15 ms and 25 ms.
They cost a few hundred bytes of the budget and they earn it twice: a bare page shows the
scheduler working with no shell to drive it, and the smoke test drives `tick()` on a synthetic
clock and asserts both the log order and the exact sequence of returned delays. The first
acceptance criterion is therefore checked against the shipping binary, not only against
`tests.wasm`. M3's shell replaces them.

M0's `coroutine_ok()` boot self-check is gone. It existed to prove the shim linked and ran; the
demo now does that far more thoroughly, and one of the two had to go.

---

## M0 — Nucleus

The first milestone: a freestanding wasm build, the `<coroutine>` shim, the allocator, the base
core types, and one line of output in a browser tab. 4,013 bytes of `kernel.wasm`, against a
32 KiB budget.

### The build command line changed in three ways

Appendix C of the concept document records a compiler invocation verified before any code
existed. Building something real against it turned up three problems, all confirmed by
compiling and instantiating modules rather than by reading documentation.

**`-Wl,--export-dynamic` is not a reliable way to export.** In a test module it exported the
mangled `operator new` and `operator delete` while silently dropping a plain `extern "C"
start()`. Whatever rule it follows, it is not "export what I wrote", and a build system whose
ABI surface is decided by linker heuristics is a bad foundation. Every export is now named
individually with `__attribute__((export_name("...")))`, wrapped in the `BRAAM_EXPORT` macro.
The `used` attribute goes with it, because `--gc-sections` would otherwise drop a function
nothing calls. The result is an export section that contains exactly what we asked for and
nothing else, which is a thing the smoke test can assert against.

**`-Wl,--allow-undefined` is not just unnecessary — it is actively bad here.** Its purpose is to
let unresolved symbols become imports. But imports are now declared explicitly with
`__attribute__((import_module("host"), import_name("...")))`, so there is nothing left for it to
resolve. Dropping it converts a whole class of mistake from runtime to link time: a stray libc
call — `strlen`, say, reached through some header we did not expect — used to become a silent
import that traps when first called. It is now `wasm-ld: error: undefined symbol: strlen`
before the binary exists. For a project whose entire premise is "we link nothing we did not
write", having the linker enforce that claim is worth more than the flag it costs.

The related worry, that `memcpy` and `memset` would leak in as imports, turned out to be
unfounded: `__wasm_bulk_memory__` is on by default for this target, so LLVM lowers them inline
to `memory.copy` and `memory.fill`. A 4 KiB struct copy produced a module whose only import was
`host.log`. No hand-written `mem*` functions are needed, and if that ever changes the missing
symbol is now a link error rather than a mystery trap.

**`--no-default-config` and `-Wl,--stack-first` are new.** The first suppresses
`bin/clang++.cfg`, which unconditionally injects `--sysroot=.../wasi-sysroot`. It is harmless
under `-nostdlib -nostdinc++`, but the whole point of using this SDK as a bare clang is that
nothing of its comes along uninvited, and determinism costs one flag. The second moves the
shadow stack below the data segment. By default the stack sits above the data and grows down
into it, so an overflow quietly corrupts globals; with `--stack-first` it grows down towards
address zero and runs off the bottom of linear memory, which traps. Concept.md §8.4 asks that
this class of bug fail loudly, and this is the same argument applied to the stack.

### The coroutine shim

Appendix C is right that libc++'s `<coroutine>` cannot be used freestanding, and right about
the shim being roughly 25 lines. One detail it does not mention, and which costs an afternoon
if missed: `std::coroutine_traits` must be defined, not merely declared. A forward declaration
compiles fine until the first coroutine, which then fails with "implicit instantiation of
undefined template". The primary template needs its body — `using promise_type = typename
R::promise_type;`.

`coroutine_handle<P>` derives publicly from `coroutine_handle<void>` rather than holding a
pointer and offering a conversion operator, which is how libc++ does it. Inheritance gives the
derived-to-base conversion for free; writing the conversion operator as well earns a
`-Wclass-conversion` warning, because it can never be selected.

`noop_coroutine` is included even though nothing uses it yet. `Task<T>`'s `final_suspend` in M1
will want it as the "resume nobody" case in symmetric transfer, and the shim is the wrong place
to be adding pieces under time pressure.

The test suite pins down more of the shim's behaviour than M0 strictly needs, deliberately. It
checks that destroying a *suspended* coroutine runs the destructors of locals held across the
suspend point — which is precisely the contract cancellation depends on in M1 (§8.1) — and that
`await_suspend` returning a handle transfers control to it, which is what makes `Task<T>`
chaining work without growing the stack.

### The allocator: spans, not headers

Coroutine frames are the hot path (§8.2), and frames are freed through `operator delete`, which
does not always know the size. The usual answer is a header word before each block recording
its size class; the usual cost is that a 16-byte allocation becomes 32 bytes once alignment is
preserved, which is a 100% overhead on the most common size.

Instead, linear memory is carved into 64 KiB **spans**, and each span serves exactly one size
class. A side table maps span index to class, so `free(p)` finds the class with
`span_class[p >> 16]`. There is no per-allocation header at all, 16-byte alignment falls out of
the class sizes, and sized and unsized `delete` are the same O(1) operation. This is the
structure jemalloc and mimalloc use, for the same reason.

Allocation within a span is a bump pointer with a per-class free list in front of it, so a
freshly claimed span costs nothing to prepare — no carving loop threading 4,096 blocks onto a
list before the first allocation can be served.

Allocations over 512 bytes take whole span runs. Their free list is address-ordered with
coalescing on insert, which is the old K&R arrangement. Coalescing is not needed for
correctness, and skipping it would have been simpler, but `Vec` growth reallocates repeatedly
and each cycle would strand a run that nothing could ever reuse. Address-ordered insertion
makes both neighbours cheap to find, and the free-run list is short in practice because small
allocations never touch it.

The span table is a fixed `u8[4096]`, capping the heap at 256 MiB. Sizing it for wasm32's full
4 GiB would cost 64 KiB of zero-initialised memory for a limit no browser tab will approach.
The array is `.bss`, so it costs nothing in the binary either way; the cap is about honesty,
not bytes, and raising it is a one-line change.

One consequence worth knowing before it looks like a bug: reserved memory grows in 64 KiB
units *per size class*. Boot reserves 320 KiB for five allocations, because a `Vec` growing
through 16, 32, 64, 128 and 256-byte capacities touches five different classes and each claims
a span. This is fine — the memory is reserved, not used, and steady-state behaviour is what
matters — but the number surprises on first sight.

### The heap base convention

Concept.md §3.4 fixes `init(heap_base)`, but in M0 the host has no way to know where the
kernel's data ends. Rather than export the layout to JS so JS can hand it straight back, `init`
treats a base of `0` as "use the linker's `__heap_base`". The signature stays as specified, the
host stays ignorant of the kernel's memory map, and M8 — where an isolated process really is
handed a base chosen by its parent — needs no ABI change.

### Errors, and the shape of `TRY`

`TRY(expr)` is a statement expression (`({ ... })`), a GNU extension that clang implements,
which is why `CMAKE_CXX_EXTENSIONS` is `ON` and the standard is `gnu++20` rather than `c++20`.
The alternative — a macro that assigns into a caller-declared variable — reads badly at every
call site, and this is a construct that will appear in nearly every kernel function.

Early return needs a value convertible to *any* `Result<U, E>`, so errors travel as a small
`ErrTag<E>` returned by `Err(e)`, which each `Result` has a converting constructor for. That is
the standard trick and it costs nothing at runtime.

`TRY_VOID` exists because `TRY` unwraps a value and `Result<void, E>` has none. Two macros is
mildly unfortunate; the alternative was making `Result<void, E>::value()` return a dummy, which
would be worse.

### Verification

Tests run headlessly under Node, which stands in for the browser perfectly well: a freestanding
module needs nothing browser-specific to instantiate. `test/run.mjs` has two modes.

The `--kernel` mode asserts the *exact* import and export lists. This looks pedantic for two
imports and two exports, but the ABI is the thing most likely to drift silently, and an
unexpected import is precisely the signature of an accidental libc dependency. The check costs
one line and catches a category of problem that is otherwise invisible until runtime.

The `--tests` mode drives `tests.wasm`, a separate binary linking the same core library. Two
binaries rather than a compile-time flag, so test code can never count against the kernel's
size budget and the number the budget checks is the number that ships.

`tests.wasm` lists its cases explicitly in `main.cpp` rather than self-registering at static
init. Self-registration needs `__wasm_call_ctors`, which `--no-entry` leaves uncalled, and that
question is better settled in M3 where the program registry (§3.6) actually depends on it.

Writing the tests found two real bugs, which is the argument for having written them:
`Str::split` read its own fields after overwriting them when the output parameter aliased
`*this` — the natural way to write a tokenising loop — and the first attempt to assert that
coroutine frames come from the kernel heap failed because clang had elided the allocation
entirely. The second is not a bug in the allocator but in the test: heap allocation elision is
a permitted optimisation, so the test now routes the frame through a `noinline` factory that
lets the handle escape, which is the situation the scheduler will actually create in M1.

### Size budget

32,768 bytes for `kernel.wasm`, from Concept.md's "~30 KB" rounded to something page-friendly.
M0 uses 12% of it. The number is deliberately not tight: its job is to make growth *visible* and
deliberate, and a budget that has to be edited every commit stops being read. CI prints the
figure into the job summary on every run, so the trend is visible without anyone going looking.

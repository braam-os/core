# Release notes

Reasoning, alternatives and trade-offs behind the code. Comments in the source say *what* a
thing is; this file says *why* it is that way. [Concept.md](Concept.md) remains the
specification — where this document and the spec disagree about intent, the spec wins and one
of the two needs amending.

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

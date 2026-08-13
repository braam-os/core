# Release notes

Reasoning, alternatives and trade-offs behind the code. Comments in the source say *what* a
thing is; this file says *why* it is that way. [Concept.md](Concept.md) remains the
specification — where this document and the spec disagree about intent, the spec wins and one
of the two needs amending.

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

# Release notes

Reasoning, alternatives and trade-offs behind the code. Comments in the source say *what* a
thing is; this file says *why* it is that way. [Concept.md](Concept.md) remains the
specification — where this document and the spec disagree about intent, the spec wins and one
of the two needs amending.

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

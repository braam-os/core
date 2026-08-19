# Release notes

Reasoning, alternatives and trade-offs behind the code. Comments in the source say *what* a
thing is; this file says *why* it is that way. [Concept.md](Concept.md) remains the
specification — where this document and the spec disagree about intent, the spec wins and one
of the two needs amending.

---

## The `braam:` prefix is gone

Everything the system writes to its own screen used to begin `braam:` — the shell's `say` and
`say2`, `exec`'s `<path>: crashed` and `no worker, retrying`, and every line boot and init print.
It says nothing: there is one system on that screen and it has already named itself in the banner
one row above. `braam: sh: too many processes` is now `sh: too many processes`, and
`braam: the shell died (status 132)` is `the shell died (status 132)`.

**What the prefix was actually drawing** is the line between the runtime speaking and a builtin
speaking, which Shell.md still records: a builtin says `<name>: <what>: <why>` and names itself,
because several of them can fail the same way and the name is the only thing that says which one
did. The runtime is not one of several. Where its message names a path or a command word, that
word is what the reader needs and now leads the line; where it does not, the sentence was always
the whole message.

**It is kept where the terminal is not the destination.** `panic()` goes through `host_log` to
the browser console, and `web/braam.js`, `web/render.js` and `web/worker.js` throw or report to
the embedding page — text landing among everything else that page logs, where the prefix is the
only thing saying who spoke. The rule is the destination, not the severity: on the Braam screen
nothing needs telling, off it everything does.

`sh_source` was the one caller that had to change shape rather than lose a literal: it printed
through `errln("braam", path, err)`, whose first argument is the speaker, and now passes the path
as the speaker with an empty middle — the form `errln` already had for a program naming a file.

`kernel.wasm` 151,542 → 151,028: seven bytes of literal in a dozen places, and one `write` per
diagnostic in `exec`.

Notes below this one quote the old prefix in their examples. They are what was true when they
were written and are left alone.

## Sixteen deep, and saying so

`SYS_PROC_DEPTH` is 16, from 8, and a spawn refused by either bound now prints
`braam: <name>: too many processes` rather than `not executable`.

**Eight was reached by hand.** Typing `sh` at the prompt costs a level, so the eighth one was
refused: init's shell is depth 0 and the check is on the child's depth, which makes the bound
"processes in a chain, counting the first" — seven nested shells and then a refusal, on a system
where a nested shell is the ordinary way to try something in another directory. The bound exists
for the fork bomb, and a fork bomb does not care whether it is stopped at 8 or at 16: the
depth cap is the second of two, and `SYS_CHILD_MAX` still holds the width at 16, so the product
is what bounds the tree either way. Doubling the one that a person walks into and leaving the one
that a bomb walks into is the whole change.

**What sixteen costs.** Nothing at rest — depth is a `u32` on `Proc`, not a table — and at worst
sixteen live instances in a chain, each a worker of its own and each about 900 KiB committed of
its 16 MB cap. The host makes workers on demand (`MAX_IDLE` is the pool's size, not a ceiling on
live ones), so a deep chain hires rather than waiting; there is no new way to deadlock. A chain
that deep is a person nesting shells or a script recursing, and the recursion is exactly what the
bound is for.

**The message named the wrong problem.** Both bounds answer `Err(NoMemory)`, and the shell
spelled out only `NotFound` and `Unsupported`, so a depth refusal fell through to `not
executable` — which points at the binary, sends the reader to `test -x` and to rebuilding, and is
false: `/bin/sh` is a program and had just run seven times. A new `Error` value would say it
exactly and is still far too expensive for a diagnostic string, for the reason the `#!` notes
below give: `Error` crosses the wire as a negated `i32` in every reply, so it means a `PROC_ABI`
bump and `web/` changes. So the shell reads the value that already arrives. The cost is that a
genuine allocation failure inside `Sys::Spawn` now says `too many processes` too — the wrong half
of one error rather than the wrong error, and the status stays 126, which is still what "found
and would not run" means.

The test is a script that runs itself: `deep.sh` recurses until the kernel refuses, and every
level above the deepest returns its status, so the error is printed once and 126 comes back to
the prompt. It is also a fork bomb with the bound taken out, which is the point of running it.

System_Calls.md said `SYS_CHILD_MAX` was 8 in prose and 16 in the table beneath; the prose was
stale and is now the table's number.

## `#!` after all

A text file beginning `#!/` or `#! /` is now a program. `./script.sh` runs, a bare `script` in
`/bin` runs, `Sys::Spawn` on one from any process runs, and `test -x` agrees. `kernel.wasm` went
149,613 → 151,542 of 262,144 and `sh.wasm` 218,887 → 219,637, the staging tree 727,600 → 729,099
of 1 MiB. **`PROC_ABI` is still 11**: argv is still argv and the host still receives
`(path, image, ProcMeta)`, so nothing a binary observes moved. A diff touching `sysabi.h` reads
like a bump and this one is not.

**This reverses "`#!` is settled rather than deferred" below**, which argued that "there is no
place to put an interpreter lookup that would not be the kernel reading file contents to decide
what a program is". The premise is right and the conclusion does not follow: the kernel already
reads file contents to decide what a program is — that is the whole of `exec_meta`, which walks
the section list looking for `braam`. Reading eight bytes further into the same image, in the same
function, against the same contract, adds no capability the kernel did not have. What the old
argument was really protecting was that `exec` should not *guess*, and that survives intact: a
text file with no `#!` is still `Err(Invalid)`, and there is no sniffing of any kind beyond the
two literal prefixes.

**Three bounds are what make it a rule rather than a search.** The interpreter is **absolute**,
because the `/` is required after `#!` — there is no PATH here and one directory is not a search
path, so a relative interpreter has nothing to resolve against and is refused rather than guessed
at. The lookup is **one level deep**, which is v7's and Linux's ENOEXEC answer: an interpreter
that is itself a script is `Err(Invalid)`. And the first line must end within `PROC_SHEBANG_MAX`,
so a file is decided by its head rather than by a scan. That cap is 128 and is `static_assert`ed
against `SYS_CHUNK`, because `test -x` decides from a single `read_chunk`: the two answers can
only agree while a whole `#!` line fits in one. Tying them at the constant is what keeps a later
raise from silently splitting `exec` and `-x` apart.

**`exec_resolve` became a two-round loop rather than a recursive call.** A nested `Task` would be
a second coroutine frame and a second allocation on the path every command takes, and "one level
only" written as a loop bound is a fact the compiler enforces rather than a convention a later
edit can drift past.

**`Executable::path` is the interpreter's now, and it had to be.** It is the key the host caches
the compiled `Module` under and it is what the no-worker backoff re-reads on each retry; keying
either on the script would hand the host a text file. The cost is that `braam: /bin/sh: crashed`
names the interpreter rather than the script — which is truthful, since the interpreter is what
crashed. Nothing a user navigates by moved: `ps`, `jobs`, `/proc` and `kill %n` all read the
scheduler's job name, which `sched_spawn` takes from the caller's word, so they still say
`./script.sh`. A second `String` holding a display name would have bought three error messages and
cost a field on every `Executable`, one of which lives on init's coroutine frame.

**A missing interpreter is 126, not 127.** Letting the interpreter's `Err(NotFound)` through would
print `braam: ./s.sh: not found` for a script that plainly exists, and would spend the one status
that means "the command word names nothing" on a file that does name something. So the second
round's `NotFound` alone collapses to `Invalid` — "not executable" — and every other error is let
through unchanged, which keeps the genuinely useful one: a stale interpreter still reports
`Err(Unsupported)` and prints `built for another process ABI`, naming the repair. A new `Error`
value was the alternative and is far too expensive for a diagnostic string: `Error` crosses the
wire as a negated `i32` in every reply, so it would mean a `PROC_ABI` bump, `web/` changes and a
System_Calls.md edit.

**A script costs one process, not two.** The resolution happens before any process exists, so
`SYS_PROC_DEPTH` and `SYS_CHILD_MAX` count the interpreter exactly as they counted a binary, and
every script shares `/bin/sh`'s already-compiled `Module` — the second and later scripts pay the
instantiation and the worker, not the compile.

**The cost, stated rather than hidden: a script's image is read twice.** `exec_resolve` reads the
whole file through the ordinary VFS before it can discover it is not a module, keeps 128 bytes of
it and throws the rest away; the interpreter then reads the same file again. Avoiding it means
splitting `read_file` into a probe-then-rest read, which is a worse VFS for files that are small
by nature. The `#!` rule lives in `sysabi.h` beside `PROC_SECTION` and `PROC_MAGIC` for the same
reason those are there — it is the exec contract — and being a header is what lets
`src/cmd/sh/condrun.cpp` answer `-x` from the one statement of it, since `braam_sh` cannot reach
`src/user/`. Being `inline` costs nothing: it is expanded into its single call site in each of the
three binaries that call it and emitted out of line in none of them.

---

## Seconds, and `-m` for the machine's unit

`sleep`, `timeout` and `watch -n` took **milliseconds**, and `vmstat` took **seconds**. Every
duration a user types is now seconds, and all four accept `-m` to mean the number is milliseconds
instead. `sleep 5` waits five seconds, `sleep -m 5` returns almost at once.

**Why the divergence was there, and why a flag keeps it.** The earlier note below — "`sleep` takes
milliseconds; there is no float parser, the scheduler is a millisecond machine, and the smoke test
needs an exact number to assert `tick()`'s return value against" — was three arguments, and only
the third is still load-bearing. The float parser is still absent and the argument is still
`parse_u32`, so seconds cost nothing to parse; the scheduler being a millisecond machine is an
implementation fact that `sleep_for(secs * 1000)` hides in one multiply. But the test harness
really does own the clock and really does assert the exact millisecond `tick()` reports, and a
whole second is a clumsy unit for a virtual clock whose ticks are hand-picked ten milliseconds
apart. `-m` is what preserves that: `test/run.mjs` spells every one of its delays with it, so not a
single tick value or hand-written `now` in that file moved when the default changed. A seconds-only
change would have had to retime four blocks and rewrite the `[30, 20, -1]` assertion that has stood
since M1.

The alternative was a fractional parser — `sleep 0.5` — which reads better than `sleep -m 500` and
was rejected on cost: it is real parsing code in four binaries that each carry their own copy of
`text.cpp`, to express something no caller in the tree wants. `sleep 0.5` is a usage error, not a
rounding.

**This supersedes two paragraphs further down.** vmstat's *"Seconds, which makes it the one time
argument here that is not milliseconds"* now describes the rule rather than the exception, and its
first clause — that a rate per second wants an interval in seconds — is the argument that won
generally. M3's *"`sleep` takes milliseconds… the divergence from POSIX lives in the usage string"*
is retired; the usage string reads `usage: sleep [-m] <seconds>` and there is no divergence left to
record.

**Nothing below the command line moved.** `Sys::Sleep`'s payload is still milliseconds (§4.3) and
`sleep_for(u32 ms)` is still the SDK's call, so this is four argument parsers and four usage
strings. `vmstat` needed slightly more care than the others: its columns are rates *per second*
whatever `-m` says, and its "a count without an interval paces itself" default has to stay a whole
second under `-m` too, since a one-millisecond interval would measure nothing but vmstat.

`watch`'s ad-hoc `args[1] == "-n"` became a leading-option loop in vmstat's style, so `-m` and `-n`
compose in either order. The three ordinary programs cap a seconds argument at 4,294,967 — as many
as convert to milliseconds inside a `u32` — and refuse anything above it rather than wrapping;
under `-m` the parser's own range is the cap.

**What the tests gained.** `sleep 5` asserting `tick()` returns 5000 is the whole seconds path in
one line, and `watch -n 100` asserting an interval of 100000 is the same check for the one program
whose conversion is not at the top of `proc_main`. The refusals — `sleep 0.5`, `sleep 4294968`,
`sleep -m` with no number, `watch -n 4294968` — are cheap and pin the boundary. Three copies of
`/share/help` are `wc`'d by an unrelated case, so lengthening four help lines moved a byte count
there — the exact number is worth keeping over a looser assertion precisely because it notices.

## The wheel is a keystroke

Scrollback has been reachable only from the keyboard since it arrived, and the gesture everyone
tries first — a wheel, a two-finger swipe — did nothing at all. Closing that cost **no import, no
export, no message the kernel understands and no mouse in the ABI**: the page turns a wheel notch
into the keystrokes the chord is already made of.

**Why not a tenth export.** `scroll(lines)` was the obvious shape and would have been exact: one
call, one repaint, no chord to invent. It was rejected because it puts a pointer gesture into the
ABI, which §3.5 spends a paragraph saying it does not have — the selection, the key bar, the
paste and the soft keyboard are all page-side, and each of them arrives as an ordinary
`{code, mods}` or as nothing at all. The wheel is not a different kind of input; it is a way of
asking for the scroll chord, exactly as the key bar is a way of asking for `Esc`. So the page
sends keys, the kernel cannot tell a wheel-born Shift+Up from a typed one, and the export list
stays at nine.

**The chord grew a row.** Half a screen a notch is unusable, so `console_pump` now takes
Shift+Up and Shift+Down beside Shift+PageUp and Shift+PageDown, one row instead of `rows / 2`.
Nothing else in the tree binds Shift with an arrow, so no claimant lost a key it was reading, and
the keyboard gains a line-at-a-time scroll it did not have — the feature is worth having on its
own, which is part of the argument for this shape over an export.

**A claimed screen keeps the fall-through, and `less` gets the wheel free.** The chord is not the
pump's while a program holds the screen, so the synthetic key is delivered raw like any other —
and `less` switches on `k.code` without looking at the modifier, so a wheel over the pager pages
it a line at a time with nothing written for it. `edit` moves its cursor a row, which is the same
bargain and harmless. Swallowing the key while the screen is claimed was the alternative: more
predictable, and it makes the wheel dead inside the two programs where scrolling is the whole
point.

**Pixels cross, rows are computed in the worker.** `deltaY` is device pixels, lines or pages
depending on the platform and the input device, and only the worker knows `cellH` — it owns the
font, which is why it also owns the geometry (`fit`). So `web/braam.js` posts the delta in device
pixels, as a selection drag already does, and `web/worker.js` divides. The leftover fraction is
**carried between events**: a trackpad delivers a few pixels at a time, and truncating each one
independently means a slow swipe scrolls nothing ever. The residue is zeroed when the direction
reverses (a reversal continues nothing) and in `fit`, where the row height it is a fraction of
changes underneath it. A page-mode delta is passed to the half-screen chord instead of being
converted, since that is what it already means.

**Fed like a paste.** The run goes through `key()` and stops when it returns false, which is the
back-pressure the ring's return value exists for (§3.5), and it is clamped to a ring's worth so a
fling cannot outrun it. One `pump()` for the whole run, so *n* rows cost one composition pass and
one `host_present` rather than *n* of each. It deliberately does not touch `key_at` or the sample
array: those measure what a *keystroke* costs, and a wheel event is not one.

**`Ctrl`+wheel is the browser's.** A trackpad pinch arrives as a wheel with `ctrlKey` set, and
zoom belongs to the user. Everything else over the canvas is prevented and taken, which is the
same claim `touch-action: none` already makes for a drag — an embedding page does not scroll under
a terminal the pointer is over. Touch is still not covered: a one-finger drag selects, and a
two-finger scroll gesture would need a decision about which one wins.

## The environment crosses a spawn

`export` set a bit nothing read. §4.5 listed it as one of six things v7 has that this cannot,
and the reason given was true: `Sys::Spawn` carried three descriptors, an argv and a cwd, and
there was nowhere to put a variable. Closing that gap cost `PROC_ABI` 10 → 11 and **no new
operation, no new import or export, and not one line of `web/proc.js`.**

**One encoding, two blobs, no length word.** An environment is a word list in exactly the format
`argv_encode` already wrote, its words `NAME=value`, so there is one codec rather than two and
`env` needs no parser of its own. The two blobs sit back to back — in `_start`'s payload and
after `Sys::Spawn`'s three descriptor words — and the join is found by walking the first
(`argv_bytes`) rather than by a length word in front. A length word was the obvious alternative
and is worse: it would sit where `argc` sits, so `argv_count(payload, len)` and the existing
assertion in `test_sysabi.cpp` would both have had to learn about it. Walking is free where it
matters, because both readers already walk the argv blob to the end. `argv_bytes` reports 0 for
anything it cannot walk, and that is unambiguous: the smallest well-formed blob is the four
bytes of an empty word list, so "malformed" and "empty" are told apart and the kernel refuses
rather than entering a child with rubbish.

**The kernel holds it, beside the cwd, and inheritance is the default.** The alternative was to
hold nothing and have `spawn()` re-serialise the caller's environment on every call, which keeps
the kernel stateless and puts the truth on the wire every time. It was rejected for what it does
to a program that starts a program: `timeout 5 env` and `watch ls` would each have had to ask
for their own environment and hand it on, and one that forgot would silently strip it. So `Proc`
gains a `String env`, `Sys::Spawn`'s op-word argument — unused until now — gains bit 0 meaning
"an env blob follows", and a spawn that says nothing hands the child the caller's. The common
case puts zero bytes on the wire. `SYS_ENV_MAX` bounds it, because a child hands its own on and
an unbounded one would grow down a chain of them.

**There is no `setenv`, and a `Sys::Env` would have failed §4.3's own rule.** Every operation has
a caller in `src/cmd/`, and this one would have had none: the shell keeps its variable table in
its own memory and builds the blob afresh at each spawn, so it never needs to ask the kernel to
change anything. A process's environment is therefore fixed at spawn — stronger than v7, which
is a fair trade for an operation nothing wanted. It also keeps the synchronous half of the ABI
closed: `getenv` would have had to be asynchronous, since the sync half is answered inside the
worker with no kernel to ask, and a `co_await` to read a variable is a bad shape.

**The runtime walks the environment rather than indexing it, and `hog` is why.** The first
version built a `Vec<Str>` in `_start` beside the argv one. That is one heap block in every
process that will never use it, and it broke `hog` — which allocates until the heap refuses,
hands one 64 KiB span back, and reports. The failure was not the sixteen bytes: it was the size
*class*. `_alloc(4)` for the reply the kernel writes back used to land in the span the argv
block had already seeded, and a bigger `_start` block moved that block to another class, leaving
no span able to serve four bytes. Walking on demand removes the allocation, which is the right
answer anyway — `Rt` holds a pointer and a length, `proc_env` is a linear scan over a handful of
words, and a program that never asks pays nothing. `hog` now hands back two spans instead of
one, and says so: reporting needs coroutine frames *and* a reply block, and those are different
size classes once nothing else is left. That fragility was always there; the environment is only
what exposed it.

**`x=1 prog` finally goes somewhere.** The note under M9 explains why an assignment prefix on a
program stage used to be expanded and then dropped — applying it to the shell would have leaked
it, and there was nowhere else. Now it goes into that child's environment and nowhere else,
which is v7's semantics exactly. A prefix *replaces* an exported variable of the same name in
the words handed over rather than arriving beside it, because a lookup takes the first match and
a second word would never be reached. Prefixes on builtin and function stages keep the
apply-and-restore they had: those run in the shell's own turn and have no spawn to ride.

**A nested shell reads its environment back into its table.** `var_init` seeds from
`proc_env_at`, marking each entry exported, so `export a=1; sh -c 'echo $a'` works and the
environment is not swallowed by the one program most likely to be in the middle of a chain.
Without it the feature would have reached every program *except* another `/bin/sh`.

**Init gives the shell `HOME` and `SHELL` and nothing else.** `PATH` is absent because there is
no search path to describe — a command word resolves as function, then builtin, then `/bin` —
and a `PATH` that did not steer resolution would be a lie a script could believe. `TERM` is
absent because §2.3's terminal is a cell grid with no escape sequences and no terminfo entry
that could describe it; `Sys::Tty` is how a program asks about the terminal, and a `COLUMNS`
would be a copy taken at spawn that the first resize made wrong. `cd` with no argument now reads
`$HOME`, falling back to the literal `/home`, which is what makes the one variable init plants
mean something rather than being decoration.

**`/bin/env` is a program, not a builtin.** Neither clause of §4's rule fits it: it does not
touch the shell's own state, and its whole cost is not the spawn — it *makes* a spawn, which is
the thing a builtin cannot do. It is the worked example for both halves of the new `spawn`
signature, and it is what makes the feature testable end to end from `run.mjs`.

---

## src/sh became src/cmd/sh

The shell is a program (§4), and it was the only program whose code did not live in `src/cmd/`.
`src/sh/` was a top-level tree beside `src/fs/`, `src/svc/` and `src/user/` — the layers *below*
userland — and being there said the shell was one of those, which stopped being true when it
became a binary. It is `src/cmd/sh/` now, a directory beside the thirty-four one-file programs,
and its entry point is `main.cpp` inside it rather than `src/cmd/sh.cpp` outside. Nothing else in
the tree gains a directory by doing this: a program that needs one is what the shell is, and
`braam_add_program` already takes a source path rather than assuming `<name>.cpp`.

**The build got shorter, not longer.** `add_subdirectory(src/sh)` left the top-level
`CMakeLists.txt` and became `add_subdirectory(sh)` at the top of `src/cmd/`'s own, so the shell's
library is declared where its binary is. `/bin/test` names `sh/cond.cpp sh/condrun.cpp` instead of
`../sh/cond.cpp` — the same two files, no longer reached for through a parent.

**`#include "sh/x.h"` became `#include "cmd/sh/x.h"`**, which is the only cost. Includes are
root-relative from `src/`, so the builtins and `src/cmd/test.cpp` and the five unit tests that
reach into the shell all name a directory deeper than before. Sibling includes inside the shell
are unchanged, because a quoted include finds a sibling first — which is why `main.cpp` says
`"shell.h"` where `src/cmd/sh.cpp` had said `"sh/shell.h"`.

`sh.wasm` is 215,270 bytes before and after, to the byte, and the three CTest cases pass
unchanged. Notes above this one name the old paths where they are describing what happened at the
time; their file *links* were repointed, so none is dead.

---

## The shell's plan is deleted, and what only it held is here

Last of the stages in `src/sh/TODO.md`, and the one that deletes it. Ten stages had landed and
each has a note of its own below, so what the plan still held was a to-do list with nothing to do
plus four things written nowhere else: **the grammar and the expansion order**, **the six
impossibilities**, **the purity boundary**, and **the size trajectory**. The first two are now
Concept.md §4.5, which the specification had been missing entirely; the last two are here. **No
behaviour changed**: `sh.wasm` went 215,238 → 215,270, and all thirty-two of those bytes are one
usage string — `unset` had taken `-f` since the functions stage and its one-line help had never
said so. The staging tree is 704,061 of 1,048,576 (67%), 277 KiB as `rootfs.zip`; `kernel.wasm` is
148,604 of 262,144, byte for byte what S7 left; `PROC_ABI` is still 10.

**Cite the stages by name, not by ordinal.** The ten notes below number themselves *first* to
*tenth*, and those ordinals are **one ahead of the plan's S-numbers** from the beginning: *"The
lexer stopped removing quotes"* took a note without taking a stage number, so the plan's S1 is the
second note, its S7 is *"The ABI changed, once"* and its S9 is *"The shell got a front door"*. The
S-numbers are kept as citations the way the worker plan's T-numbers were — CLAUDE.md and the
commit messages say "S7" and mean the `Sys::Dup` stage — but a heading is the unambiguous name and
this note is the last place the two schemes have to be reconciled.

**The purity boundary is a standing rule, not a stage's tactic.** `parse.cpp`, `tokenize.cpp`,
`expand.cpp`, `match.cpp` and `cond.cpp` touch nothing but `Str`, `String` and `Vec`, and
`test/CMakeLists.txt` compiling them into `tests.wasm` **is** the enforcement: a syscall in any of
the five is a link error, not a review comment. It cost real design twice and both times the
answer was a second file rather than a weakened rule — the directory walk could not go in
`expand.cpp` and became `glob.cpp`, and `test`'s file primaries could not go in `cond.cpp` and
became `condrun.cpp`, which answers the probes the pure evaluator names. That is what lets
`test_parse.cpp`'s one-string `shape()` compare check a whole parse, and `test_cond.cpp` check a
whole expression grammar against a table of canned answers, with no kernel in the loop. Anything
new that reaches `exec_node` goes the same way.

**The trajectory, since it is the only place the arithmetic is recorded.** Per stage, in bytes of
`sh.wasm`: 20,807, 10,644, 12,678, 14,607, 8,699, 13,897, 20,650, 26,270, 3,501. About twenty
bytes of wasm per line of C++, which the arena stages came in under and the coroutine ones over,
and the estimate was calibrated against the binary after S1 rather than guessed per stage — which
is why nine stages landed inside a budget that never moved. S8 also added `/bin/test`, 19,246 of
the tree on its own, because a builtin of the second kind keeps its file.

**The integration cases are the last thing the per-stage tests could not do.** Every block in
`run.mjs` before this one stays inside one stage, so nothing checked that the features *compose*.
What checks it now is a planted script that does something real — a function, an EXIT trap, a
glob walked by `for`, a `case` that skips an arm, a `$( )` with a redirection inside it, a
here-document and a nonzero exit — asserted whole, and it is the same script Programming_Manual.md
prints, so the manual's example cannot rot. Beside it are the seams: a function body that globs
with the *call* redirected, `case` inside `while` inside a function, a `for` over a substitution
under `set -e`, a builtin at each end of a pipeline with a program between, `set -x` tracing
inside a compound, and a construct through `sh -c`. All of them passed the first time they were
run, which is the finding — the stages were composable as they landed, and this proves rather
than fixes it.

**Three instances at the peak, however many turns the loop takes.** The script's cost is asserted
rather than described: this shell, the script's own, and whichever program is running. A shell
that leaked an instance per turn, or spawned the loop body in parallel, would fail that line — and
it is the counterpart to S8's assertion that a loop of builtins hires no worker at all.

---

## The shell got a front door

Tenth of the stages in `src/sh/TODO.md`: `sh <file> args`, `sh -c cmd`, `-e -x
-u` at startup, `$0` and the positional parameters a shell begins with. `sh.wasm` went from
211,737 to 215,238 bytes and the staging tree from 700,528 to 704,027 against an unchanged 1 MiB
budget — 3,501 bytes over about 150 lines, the cheapest stage of the ten. **`kernel.wasm` did not
move**: 148,604 of 262,144, byte for byte what S7 left. The syscall ABI did not change and
`PROC_ABI` is still 10.

**A script file is read whole and parsed once, because that is what `.` already did.** The
alternative was the loop `sh -s` runs — a `LineReader` over the file, accumulating until
`line_incomplete` says the construct closed — and it is what v7 does, executing as it parses. Two
things decided against it. The smaller is duplication: `sh_source` already reads a file with
`read_file` and hands the text to one parse, so `sh file` and `. file` are now the same
mechanism and cannot drift apart. The larger is stdin. A shell reading its script off stdin has
that stream open for two purposes at once, which is exactly the fidelity loss S8 recorded for
`read`; a script named on the command line has no such conflict, and `while read l; do …; done`
inside one reads the shell's actual stdin. The cost is stated rather than hidden: a syntax error
anywhere in the file means none of the file runs, where v7 would have run everything above it.

**`sh -c cmd name args` names `$0` itself, and the arithmetic is v7's.** It is tempting to make
every word after the command string a positional parameter and leave `$0` as `sh`, and that is
wrong in the way that only shows up later: `options()` and `main()` in the v7 port compute
`cmdadr = dolv[0]` uniformly across all three entry points, so `$0` is the script for a file, the
first operand for `-c`, and argv[0] otherwise. Following it means `sh -c 'echo $0 $1' a b` prints
`a b` here as everywhere else, and `sh -s a b` puts `a` at `$1` rather than eating it. The
implementation is `OptParse` plus nine lines, since `var_init` plants `$0` and `args_set` carries
it over — the shell's own state was already the right shape and no new storage was added.

**`${x?}` now ends a non-interactive shell, which discharges a deferral.** *"The shell got
variables"* below records that `${x?}` "aborts the line rather than the shell … because
script-entry policy belongs to the stage that adds `sh file` and `sh -c`". This is that stage,
and v7's answer is taken: a script stops where the parameter is missing rather than running on
past a hole, while at a prompt only the line is abandoned. It reuses `Flow::Exit` and so adds no
exit path, exactly as `set -e` did. Folding the decision in also collapsed four hand-copied
expansion-failure epilogues in [job.cpp](../src/cmd/sh/job.cpp) into one `expand_failed`, which is
where the `^C`-is-130 distinction now lives once instead of four times.

**`#!` is settled rather than deferred.** `exec_meta` requires `\0asm` and a `braam` custom
section carrying `PROC_ABI`, so a text file can never be handed to `Sys::Spawn`; there is no
place to put an interpreter lookup that would not be the kernel reading file contents to decide
what a program is. `sh file` and `sh < file` are the whole of it, and `./script.sh` will not
arrive later — **which it since has: "`#!` after all" above reverses this, and says why.**

---

## The rule grew a second clause

Ninth of the stages in `src/sh/TODO.md`: `test`, `[`, `:`, `read`, `wait`,
`trap` and `set -e -x -u`, plus `echo`, `true` and `false`, which S8 as written had ruled out.
`sh.wasm` went from 185,467 to 211,737 bytes and the staging tree from 654,945 to 700,528 against
an unchanged 1 MiB budget — the extra being `/bin/test`, a new binary at 19,246. **`kernel.wasm`
did not move**: 148,604 of 262,144, byte for byte what S7 left. The syscall ABI did not change
either, and `PROC_ABI` is still 10.

**`test` is a builtin, and the reason is not the one TODO.md wrote down.** The plan proposed
"the shell's own state **or the condition of a loop**", drawn narrowly enough to admit `test`,
`[` and `:` and to exclude `echo` by name. That wording does not survive the first request to
add `echo`, and it was never the real criterion: what makes `test` worth having inside the
process is that a program costs an instantiation and a worker — roughly a millisecond, §4.4 — and
`test` is two hundred lines of pure logic. The cost of running it *is* the spawn. So the second
clause is that, and it admits exactly the six commands small enough for the statement to be true:
`test`, `[`, `:`, `echo`, `true`, `false`. A `while [ … ]; do echo …; done` paid two workers a
turn and now pays none, which `run.mjs` asserts by counting links across a five-turn loop.

**The file in `/bin` stays, and that is the other half of the amendment.** A builtin of the first
kind has no file and never will — there is nothing for `/bin/cd` to do. A builtin of the second
kind is a *shortcut*, so its file is still the thing anything spawning by path gets: `/bin/test`
now exists for a future `find -exec`, and `/bin/echo`, `/bin/true` and `/bin/false` were not
deleted. TODO.md argued against shipping `/bin/test` as duplication for nothing; the answer is
that the duplication is the point, since the alternative is that adding `find` later means adding
`test` back. `help` prints each name once — the builtin's line, because that is what typing the
name runs.

**The expression is pure and the probing is not, and they had to be two files.** v7's `test` is
five mutually recursive functions, and three of its primaries — `-r -w -x -f -d -s -t` — have to
ask the filesystem. Making the recursion coroutines would have cost a frame per nesting level
against an allocator whose top size class is 512 bytes. Instead [cond.cpp](../src/cmd/sh/cond.cpp)
walks the expression twice: once naming every file primary, then, with the answers in hand, once
more evaluating. It works because **v7's `-a` and `-o` are the bitwise `&` and `|`** rather than
the short-circuiting pair, so both sides of every operator always evaluate and the two walks
consume the same tokens in the same order. The indices line up by construction, not by agreement
between two scanners — which is why collecting and evaluating are one function with a flag rather
than two. `cond.cpp` reaches no syscall, so it joins `parse.cpp` and `expand.cpp` on the purity
boundary and `test_cond.cpp` checks the whole grammar against a table of answers.
[condrun.cpp](../src/cmd/sh/condrun.cpp) is what goes and looks; it touches no shell state, which is
what lets `src/cmd/test.cpp` build the binary out of the same two files rather than link
`braam_sh`.

**Two primaries answer out of the gaps rather than out of a mode word.** There are no file
permissions and OPFS stores none, so `-r` is "it exists": everything readable that is there. `-w`
is v7's `tio(a, 1)` unchanged — open it for writing and close it again — which asks the one
question that has an answer here, since `vfs_open` refuses `O_WRITE` on a read-only mount. It
also inherits v7's answer for a directory, which is *false*, because opening one for writing
fails there too. TODO.md proposed deriving it from `/proc/mounts` instead; the open probe is
shorter, exact, and needs no path normalisation. `-x` is true for a file whose first four bytes
are `\0asm`, because that is what `exec_meta` requires and therefore what executable means here.

**`read` is where the shell reads past what it was asked for.** `Sys::Read` carries no length —
the reply is a whole chunk, whatever the writer wrote — so there is no reading a line and leaving
the rest. A file-scope `String` holds what followed the newline, keyed by the descriptor it came
off, and a `read` on a different one drops it. Without that, `while read l; do …; done < file`
would see one line and stop, since a three-line file arrives in a single chunk. The cost is that
`sh -s` reading a script off stdin has its own `LineReader` buffering separately, so a `read`
inside such a script sees a different position in the same stream; that is a real fidelity loss
and it is the price of a length-free read.

Interactively `read` needs nothing at all, which is worth recording because it looks like it
should. `exec_pipeline` gives the keyboard back before any builtin runs and takes it again after,
so for exactly the window a builtin is awake the console pump has no raw claimant and is cooking
lines into this shell's stdin — and doing the echo.

**`wait` puts the job in front, and v7's does not.** There are no signals, so being in the
foreground set is the only way a `^C` can reach anything; a `wait` that did not would park the
shell on a job with no way out. That makes `wait %n` the same call `fg %n` makes, minus the echo
of the command line, and `builtin_fg` is now the thin one.

**`trap` has two signals because there are two things that can happen.** `trap … 0` fires from
the `shell()` funnel, whatever ended the loop. `trap … 2` fires from the one place a `^C` becomes
a status — `exec_pipeline`'s epilogue, where 130 already becomes `Flow::Interrupt` — and only in
an interactive shell, where the interrupt went to the stages and this process was never
cancelled. In a script the process itself is cancelled and `CancelState::cancelled` is sticky, so
every await from that point answers `Err(Cancelled)` and a handler could neither spawn nor write.
It is accepted rather than worked around, and `trap '' 2` is **refused outright** for the same
reason: nothing can decline a cancellation that has already been delivered, and accepting the
syntax while ignoring it would be worse than saying so. A trap is *taken* rather than read before
it runs, so an action cannot fire itself and a `trap` inside one replaces it.

**`set -e` needed a counter, not a flag.** The rule is not "a nonzero status ends the shell" but
"a nonzero status that nobody asked about ends the shell", and the four places that ask are an
`if` condition, a loop condition, `!`'s operand and every element of an `&&`/`||` chain but the
last. `Ctx` gained a `cond` depth incremented around exactly those, and the test itself sits at
the end of `exec_node`, where every node's status passes through once. It reuses `Flow::Exit`, so
`set -e` adds no exit path — `exit` already owned that one.

**`set -u` is a field on `Vars`, not a callback.** A lookup cannot tell a bare `$x` from the one
inside `${x-y}`: only the walk knows that the operators asked first and are exempt. Making it
data keeps [expand.cpp](../src/cmd/sh/expand.cpp) pure, so the rule is checked in `test_expand.cpp`
with the rest of the expander. The check went into `named` and `positional` rather than `emit`,
because a bare `$x` reaches those two straight from `dollar()`.

**One gap got wider, and it is the price of the second clause.** `while true; do echo x; done`
used to be interruptible, because `true` and `echo` were programs and the shell armed them with
`Sys::Fg`. Both are builtins now, so that loop has no child in the foreground set, no `^C` reaches
anything, and `rt.h` is explicit that nothing cancels from inside a process. A loop with a program
still in it — `while true; do sleep 100; done` — is interrupted in one press, and the escape from
the other kind is still killing the shell, which init replaces. The alternative was leaving `true`
a binary and paying a worker a turn for the one shape of loop that most needs not to; the gap is
recorded rather than traded for.

`$-` came with the letters, and cost `-` its place as an ordinary byte: it is a special
parameter now, so `$-` is the options and no longer a literal `$-`. The flags are one word, which
is what makes them one more field on `( … )`'s checkpoint — TODO.md's subshell row promised
`(set -e; …)` isolation and this is what pays it.

---

## The ABI changed, once

Eighth of the stages in `src/sh/TODO.md`: here-documents, `>&`, `exec`
redirections, inherited base stdio and `( … )` as a state checkpoint. `sh.wasm` went from 164,817
to 185,467 bytes and the staging tree from 634,295 to 654,945 against an unchanged 1 MiB budget.
**`kernel.wasm` moved for the first time in this plan** — 148,483 to 148,604 of 262,144 — because
this is the stage that had to add an operation.

**`Sys::Dup`, and it was `exec` rather than `2>&1` that forced it.** The plan of record promised
no syscall changes, and that promise held for seven stages. What breaks it is not the obvious
thing: `cmd 2>&1` already worked, since `ChildIo{.err = SYS_STDOUT}` is resolved by the kernel
against the parent's own stream, and a builtin merging two streams is one integer assignment. It
is `exec >file`. `Spawn` **moves** a descriptor — that is what closes a pipe's write end and gives
the reader its end of input — so the shell's own base descriptor would be gone into the first
child that ran. Nothing in a thirty-seven-operation table can stand in for a duplicate.

It cost less than the promise suggested. Ops 25–31 were free before `Sleep = 32`, so `Dup` renumbers
nothing; a new operation is an enum value on each side rather than a new import, so `smoke`'s
exact per-binary import surface stayed green untouched, which is the whole point of multiplexing
`sys_async`. `PROC_ABI` went 9 → 10 and `exec_meta` refuses anything older, so a stale binary is a
diagnostic.

**One rule had to be relaxed, and it turned out to be a rule that never said what it meant.**
`Spawn` refused to move a handle with `refs > 1`, and its comment said "nothing this process is
inside a syscall on". Those are different things the moment a second *descriptor* can exist: every
dup'd fd would have been unspawnable. The condition it always meant is recorded on the handle as
`busy_r`/`busy_w`, so the test moved there and the comment finally describes the code. `Handle`
also gained an `fds` count beside `refs`, because closing one of two descriptors must shut
nothing.

**The here-doc cliff did not exist.** TODO.md specified a `/tmp` file with a unique name, a path
tracked on `Run`, a field on `JobEntry` and a reap in `jobs_report`, all to avoid "a hard cliff at
8 × 512 = 4,096 bytes". That is the `PIPE_SLOTS` misreading corrected at S5, a second time:
`pipe_write` puts a whole `Str` into **one** slot, and a single `Sys::Write` carries up to
`SYS_STAGE_MAX` = 1 MB. So a here-doc is a pipe — write the body, close the write end, hand the
read end over — and every piece of the temp-file machinery was deleted before it was written.
Two stages in a row have now been simplified by reading `PIPE_SLOTS` correctly; the comment on it
says "chunks, not bytes" and means writes.

**The body is collected at parse time and nothing else may be.** `line_incomplete` re-parses the
whole accumulated text on every line the user adds, and `run_line` parses it again — so collection
has to be a pure function of the text. Creating the pipe in the parser would have made one per
keystroke line. The body is stored as the redirection's *target*, because `Tree`'s target table is
an arbitrary byte store rather than a filename table, and the pipe is made where the other
descriptors are.

Two rules fall out of the accumulator's shape rather than from any specification. The text never
ends in a newline — `shell.cpp` joins lines with one — so a delimiter at the very end of the text
must count as terminated, or the prompt asks forever. And an unterminated body has to fail with
`more` set, which is what puts `PS2` on the screen.

**A here-doc expands but does not unquote**, and that needed no change to the expander. The body
is escaped — every `'`, `"` and `\` gets a backslash — before `expand_one` sees it, so `$` acts
and the quotes come back out as themselves. A quoted delimiter skips even that.

**Pipes now run before redirections, and that is a fix, not a rearrangement.** `ls /nope 2>&1 | wc`
must send stderr into the pipe, and `2>&1` copies whatever `out` holds *at that moment* — which
was still the terminal, because the pipe was attached afterwards. The order is now pipes,
redirections, capture. `a > f | b` is unchanged: the redirection displaces the write end and
closes it, so `b` still gets a clean end of input.

**`ours()` is the whole of what changed for base stdio.** It decided what a stage may close by
`fd >= SYS_FD_MIN`, and a base descriptor of 7 breaks that assumption everywhere at once — the
replace-on-second-redirection, the per-builtin close, the final sweep. It takes the base now, and
the three hard-wired constants in the pipe and capture tests became comparisons against it. That
is the entire mechanism, and it lifts S6's function refusal for free: `f | wc` and `f > log` work
because `call_func` already ran the body on the caller's `Ctx`, which now carries the stage's
descriptors.

**A compound may be redirected but still not piped**, and the split is where the grammar is. A
compound's own redirections ride on a synthetic `Command` named by the node's spare `d` field, so
`{ a; } > f` and `for … done > f` needed a parser arm and an `exec_node` prologue. Piping one is a
different change: a `Pipe` node's stages are *commands*, and making them nodes is its own work.
The refusal narrowed rather than went.

**`( … )` restores everything but the job table.** cwd through two syscalls, the positional
parameters through the `args_swap` `call_func` already used, the variables through a new
`vars_swap`, and the function table through a snapshot that holds each body's tree — a
redefinition mutates an entry in place rather than growing the vector, so a mark-and-truncate
would have missed it. `(exit 3)` reports 3 and the shell lives, caught at the boundary the way
`call_func` catches `Flow::Return`.

The job table is deliberately shared: the children are this same process's and must still be
reaped, so restoring it would leak kernel child slots and break `jobs_report`. And `exec` *is* in
the checkpoint, which is not obvious until you look for the way back — there is no `/dev/tty` and
no way to name the stream the shell was handed, so `exec >file` at the top level is irreversible.
Inside `( … )` it is not, and that is the only place it can be tested.

**`>&-` is refused rather than faked.** Saying "this stream is closed" needs a value the Spawn
payload does not have and a null *sink* the kernel does not have — `null_source` exists for input
only. A gap to record.

**What the tests prove separately.** `test_parse.cpp` renders a here-doc's collected body as one
string compare, including two on one line taken in order, `<<-`'s tabs, a quoted delimiter, and
both unterminated forms asking for more. `test/run.mjs` has what the tree cannot show: a here-doc
typed over three lines with `PS2` on the screen, `2>&1` reaching a pipe and a file, a compound and
a function redirected, and a subshell putting back a cwd, a variable, the parameters, a function
and a redefinition.

---

## A line could outlive itself

Seventh of the stages in `src/sh/TODO.md`: `name() { … }`, `.`, `eval`,
`return` and `unset -f`. `sh.wasm` went from 150,920 to 164,817 bytes and the staging tree from
620,398 to 634,295 against an unchanged 1 MiB budget. **`kernel.wasm` did not move and the syscall
ABI did not change.**

**This is the first stage with no reference to port.** The V7 shell has no functions and no
`return` — both arrived with SVR2, five years later — so where every stage before this one could
be checked against `/Users/vak/Project/Besm-6/v7besm/cmd/sh/`, this one is POSIX by choice. What
v7 *does* have is `.` and `eval`, and its shape was worth keeping: one `execexp()` that pushes an
input source and runs it in the current everything. `run_text` is that function, and `.` and
`eval` differ only in where the text comes from.

**A function body outlives its line, and that is the whole of the difficulty.** Every stage until
now could assume a parse dies with the text it came from. The tree is already heap-allocated —
S3 put it there — so the change is a count, not an allocation. What it could *not* be is a
destructor: `Tree`'s move constructor is implicit, `test_parse.cpp` asserts a frozen tree still
moves, and declaring `~Tree` would delete that move and fall back to the deleted copy. So the
count is a plain `mutable u32 refs` with `tree_hold`/`tree_release` beside it, and a stack `Tree`
— `line_incomplete`, the tests — never touches either.

**The second half of that lifetime is the text, and it is easy to miss.** `Tree::line_` is a
*view* of the line, not a copy, and `text()` over it is what `jobs` lists. A tree pinned by a
function outlives its line, so `own_source()` copies the text into the tree and repoints the view
— called only when `defines()` says a `FuncDef` was added, which `add_node` records as it goes.
Without it a background job started inside a function would list freed memory, which is the kind
of bug that shows up as a garbled `jobs` months later.

**Telling `f() { … }` from `f arg` needs one token of lookahead the parser did not have**, and
`simple()` adds the name to the store on its first iteration with no way to take it back. So
`pipeline()` eats the name itself and hands it on: to `func_def` if a `(` follows, to
`simple(name, true)` otherwise. Two orderings are load-bearing. `compound()` keeps first refusal,
because **every keyword is also a name** — without that, `case x in …` had `case` eaten as a
function name and forty-nine tests failed at once. And a `(` with anything but `)` after it is
reported as `syntax error near '('` rather than `expected ')'`, so `echo (x)` still says what it
said before: it was never a definition.

`(` and `)` cost nothing here, having become tokens at S4 for `case`'s arms. That is twice now
that stage has paid for itself.

**The body runs on the caller's `Ctx`, which is a deliberate trade.** It is what gives the body
the caller's capture, keyboard and depth bound for free — so `x=$(f)` works with nothing added,
and a runaway recursion reports `too deeply nested` rather than overflowing the wasm stack. The
price is that a function is not a scope: `break` inside one reaches a loop outside it, and its
variables are the shell's. There is no fork, so the alternative is the save-and-restore checkpoint
S7 owes `( list )`, and doing it twice is how the two would drift.

**What the call does save is the positional parameters**, and `var.cpp` gained one primitive for
it: `args_swap` exchanges the whole block and hands back what was there. `$#` is derived from the
vector's size, so restoring one restores the other.

**`Flow::Return` was almost free.** `loop_step` already passes anything that is not `Break` or
`Continue` straight through, so a `return` unwinds `while`, `for`, `case`, `if` and `{ }` with no
change to any of them; only the call site notices it and clears it back to `Normal`. `Ctx::frames`
counts function calls and sourced files together, so `return` outside both is a silent no-op —
the rule `break` outside a loop already follows.

**A function is looked up before a builtin and before `/bin`**, which is ksh's order and POSIX's.
It costs three edits, all at gates `Stage` already had. What it does *not* yet get is a pipe or a
redirection: the body would write to the shell's own stdout, because `exec_pipeline`'s base stdio
is still hard-wired, so `f | wc` is refused with the message `{ a; } | wc` has had since S3. S7
threads the base stdio and lifts both at once.

**`ShIo` still has no `Ctx`, and did not need one.** `.` and `eval` reach the walk through
`sh_source` and `sh_eval` in `job.h`, over a file-scope pointer to the walk in progress that
`exec_pipeline` saves and restores around each builtin. That is the shape `loop_break` and
`shell_exit` have had all along, and it kept `Ctx` private to one file rather than publishing a
type that two builtins out of sixteen need.

**One test bug worth recording, because it is a property of the feature.** A case proving a
function shadows `/bin` defined `echo() { true; }` — and a definition outlives its line, so every
later case in the file silently printed nothing. Both shadowing cases now `unset -f` on the same
line. A language where definitions persist needs its tests to clean up after themselves, which no
earlier stage did.

**What the tests prove separately.** `test_parse.cpp` renders `fn(name){body}` as one string
compare per form — no space needed before `(`, a newline before the body, any compound as the
body — plus the three refusals. `test/run.mjs` has what the tree cannot show: that `$1` and `$#`
are the call's and go back afterwards, that a redefinition wins, that `return` carries a status
out through a loop, that a function shadows both a builtin and `/bin`, that `unset -f` really
removes one, and that a function defined in a sourced file is still callable after the `.`
returned.

---

## A word could run a command

Sixth of the stages in `src/sh/TODO.md`: `$( )` and the backtick, the pipe the
shell drains itself, and the hook that lets a pure expander reach an impure command. `sh.wasm`
went from 142,221 to 150,920 bytes and the staging tree from 611,699 to 620,398 against an
unchanged 1 MiB budget. **`kernel.wasm` did not move and the syscall ABI did not change** —
`Sys::Pipe` has been there since M6, and until now `watch` was its only caller.

**The expander gives up rather than becoming a coroutine.** Running a command needs a `co_await`;
`expand.cpp` is synchronous, and it is compiled into `tests.wasm` precisely because it reaches no
syscall. So `Vars` took a fifth callback that hands back what a substitution *printed*, and when
nobody has that yet the walk abandons the whole word with `Err(Again)`, naming the command and the
byte where it began. `job.cpp` runs it, files the output under that offset, and expands again.

That is only correct because the walk is idempotent, and it is idempotent for one specific reason:
`${x=$(a)}` gives up **inside** the operator's body, before `v.set` is reached, so the retry finds
x still unset and takes the same branch. Every other operator only reads. The word is walked once
per substitution in it — one or two in practice — and the alternative, collecting every
substitution in a single pass and re-expanding once, is what does *not* work: it would have to
commit a placeholder for `${x=$(a)}`, and the second pass would then see x set and take the other
branch.

The property the TODO asked for falls out with nothing added: `${x-$(cmd)}` runs the command
exactly once and only when the branch is taken, because the operator's word is only ever walked
on the branch that takes it. `test_expand.cpp` proves it with a call counter rather than by
reading the code.

**The offset that keys the memo needs no plumbing.** Every recursion in the expander — a `"…"`
body, a `${x-…}` body — is over a *substring* of the raw word, so subtracting the two data
pointers gives the absolute offset and no base has to be threaded through `run`, `dollar` and
`braced`. The unit tests assert it through both nestings, which is the only way that trick stays
honest.

**TODO.md's reason for `ShIo::capture` was wrong, so the field was not written.** It said a
builtin inside `$( )` would "fill eight chunks with nobody left to drain them". `PIPE_SLOTS` counts
**writes**, not 512-byte chunks: `pipe_write` puts a whole `Str` into one slot and returns its
full size, so a builtin that buffers and writes once — which is `builtin.h`'s standing rule, and
what all six output builtins do — uses one slot of eight and never parks. The last stage takes the
capture pipe whether it is a builtin or a program, `builtin.h` and its six call sites did not
change, and the rule there gained the second reason it is now load-bearing for. `builtin.h` also
had "eight chunks" and now says what it counts.

**Drained before the wait, and `watch` had already written the paragraph.** A child parked in a
full pipe has not exited, so collecting statuses first is a deadlock only `^C` could break. The
drain goes between the builtin loop and the wait loop, which is the one seam where the shell holds
the read end and nothing else: a program stage's write end was *moved* into it by `spawn`, and a
builtin's was closed by `close_stage` the moment its turn ended. `run.mjs` runs 7,077 bytes
through it — fourteen chunks against eight slots — which is the case that hangs rather than fails
if the two are ever swapped.

One capture pipe per *pipeline*, not per line. `$(a; b)` makes one for each and drains each before
the next runs, so the outputs concatenate in order and at most one message is ever queued; one
pipe for the whole list could not see end-of-input until the list was over.

**`"$(echo "a b")"` forced a real fix.** A `$(…)` inside double quotes quotes independently, and
both scanners looked for the closing `"` byte by byte — so the inner quote ended the outer run and
the word broke in the wrong place. The lexer's scan and the expander's now skip a `$(…)` whole,
using the same `paren_end` they use everywhere else. `brace_end` was already the precedent for
sharing a scan rather than keeping two copies; `paren_end` and `tick_end` join it in
`tokenize.h`.

**A substitution is not a subshell, and cannot be.** There is no fork, so the command runs in this
shell: `$(cd /x)` moves it and `$(y=1)` sets a real variable. What *is* contained is containable
without one — the command is parsed and run against its own `Tree` and its own `Ctx` rather than
through `run_line`, which would clear a pending `break` and let `$(exit)` end the shell. The inner
`Ctx` carries the outer's depth, so `$($($(…)))` is bounded like any other nest, and inherits
`interactive`, so `^C` reaches the command and comes back as `Err(Cancelled)` — which the
expansion path already turned into 130 at S4. The rest is in CLAUDE.md's gaps, waiting for the
checkpoint S7 has to write for `( list )` anyway.

**Trailing newlines go, which is not a detail.** `x=$(pwd)` must not put a newline in x, and every
shell since v7 strips them. Everything else about the output falls out of `put_value`: unquoted it
splits against IFS and is left **unmarked**, so a `*` out of a command globs exactly as one out of
a variable does; inside `"…"` it is marked and unsplit. That is one call, and it is the whole of
the quoting behaviour.

**Nothing bounds the captured output** but the process's 16 MB cap, which is v7's answer too.
`watch` stops appending at 32 KB because it only paints a screen; a substitution that truncated
would corrupt a value with no way to tell.

**What the tests prove separately.** `test_tokenize.cpp` has the balanced scans — nesting, a `)`
inside quotes, a backtick run inside double quotes, and both unterminated forms reaching the
continuation prompt. `test_expand.cpp` has the hook against canned answers: the splitting, the
mark, the stripped newlines, the call counter, and `Err(Again)` reporting the right offset through
a quote and through an operator body. `run.mjs` has what neither can: `$(ls /bin | grep less)`
through a real pipeline, a builtin down the same pipe, a nested substitution, `$(pwd)` with no
newline, and the 7,077-byte drain.

---

## A word became a pattern

Fifth of the stages in `src/sh/TODO.md`: `*`, `?`, `[a-z]` and `[!…]` over the
real store, and `case`/`esac` with its arms. `sh.wasm` went from 127,614 to 142,221 bytes and the
staging tree from 597,092 to 611,699 against an unchanged 1 MiB budget — 58% of it. **`kernel.wasm`
did not move and the syscall ABI did not change**; a directory listing is `Sys::List`, which `ls`
has been calling since M5.

The two features ship together because they are one matcher. A `case` pattern *is* a glob pattern,
and writing the matcher twice is how the two would disagree about what `[a-` means.

**This is the first reader the quoting mask has, and the mask was right.** S1 built `Field::mark`
one byte per byte with these two consumers named in the header and nothing using it for two
stages; `test_expand.cpp` has asserted since then that `$star` yields an *unmarked* `*` where
`'*'`, `"*"` and `\*` yield a marked one. Globbing needed no change to the expander at all —
`glob_match` takes the mark as its second argument and a marked metacharacter matches itself. The
alternative, deciding at expansion time whether a word "looks like a pattern", is what shells that
have no mask do, and it is why they cannot tell `$x` holding a star from a typed one.

**The walk could not go where the plan put it.** TODO.md said the directory walk belongs in
`expand.cpp`. It cannot: `list_dir` is a coroutine, and `expand.cpp` is compiled into `tests.wasm`
precisely because it reaches no syscall — the link is what enforces that, not a promise. So the
matcher is `match.cpp`, pure and in `tests.wasm` beside the parser and the expander, and the walk
is `glob.cpp`, in `braam_sh` alone. The purity boundary is the thing being protected here, and it
survived by splitting the file rather than by weakening the rule. S5's `substitute`/`glob` pair
is the same argument one step further on.

**The matcher does not follow v7's `gmatch`, and the reason is the pattern the tests name.** V7
recurses once per `*`, and the port this work follows already turned its other three tail calls
into loop iterations. Both forms are exponential in *time* on `a*a*a*b`: every star re-tries every
suffix independently. A single `*` class is the whole of this pattern language, so one saved
backtrack point — the last star and the offset it was tried at — answers it, in a loop, with no
recursion left to bound. `test_match.cpp` runs `a*a*a*b` against thirty `a`s, which the recursive
form would not finish.

**Three answers where v7 and POSIX differ, decided the way S3 decided its three.** A leading dot
must be asked for, and a *quoted* dot counts: `\.*` lists dotfiles here where v7 lists nothing,
because every shell since has done so and the rule is shorter to state. An unterminated `[`
matches nothing rather than being taken literally — that is v7's answer, and it costs nothing
visible, since a word that matches nothing is used as it was written. And a component that is not
the last must be a directory, so `echo */` lists directories and `a*/b` never tries to descend
into a file.

**No sort was needed, which the plan did not expect.** `vfs_list` has sorted its entries in
case-free byte order since M5 — `ls` relies on it and says so. Prefixes are walked in order and
each listing arrives ordered, so a multi-component glob comes out ordered without a comparison
anywhere in `glob.cpp`. TODO.md had budgeted an insertion sort for it.

**`(` and `)` became operators, which is S7's lexer work done early.** A `case` arm ends in `)`,
and until now that was an ordinary word byte — `a)` lexed as one word. The parser could have
stripped the trailing byte inside `case_clause` and left the lexer alone, but that misreads
`case x in a\)` and cannot accept POSIX's optional leading `(`. So `is_operator` took both, and
the price is that `echo (x)` is now `syntax error near '('` instead of printing `(x)`. Every real
shell says the same thing, and `( list )` — which S7 has to run as a state checkpoint, there being
no fork — now lexes already. `parse()`'s trailing diagnostic stopped saying "unexpected keyword"
for a token that is not a word, so `;;` alone reports itself rather than being called a keyword.

**Only argv words are globbed.** An assignment value and a redirection target expand under
`Split::One` — one field in, one field out by definition — and globbing them would break that
contract for no gain worth having; `> *.txt` is a refusal to write into a pattern rather than a
silent single match. `for`'s word list *is* globbed, which cost one call, because S3 put its words
through the command expander for exactly this.

**`expand_all` became a coroutine, and that is the whole of the cost to the job runtime.** It
expands a command's raw words into a scratch vector and hands that to `glob_fields`, which appends
to `Run::words`; `argv0[i]` is still pushed before the command's words, so the slice arithmetic
nothing else knows about did not change. A `^C` caught mid-listing arrives as `Error::Cancelled`
out of the expansion, which is the one error that must not print "out of memory" — it reports 130
and abandons the line, through the path a stage reporting 130 already took.

**`case` is not a loop and must not look like one.** Its arms are a flat run of (patterns, body)
pairs in the child list, as `if`'s branches are, for the third-and-fourth time the same reason:
in this executor depth costs coroutine frames. It never touches `cx.loops`, so a `break` in an arm
belongs to whatever loop is above it, and a `case` that matches no arm reports 0 with an explicit
`var_status(0)` — the rule `if` needed when it took no branch, since nothing ran and so nothing
published a status. v7's literal-compare fallback after a failed `gmatch` is not reproduced: a
marked metacharacter already matches itself, which is what that fallback was for.

**A generated name carries a mask of its own.** `Field` promises one mark byte per text byte, and
a name off the store has to keep that promise even though nothing reads it again — it is marked
literal throughout, so no later stage can take a `?` in a filename for a pattern.

**What the tests prove separately.** `test_match.cpp` is the matcher alone against table literals,
including the quoted star, the quoted `-` inside a live class, the unterminated `[`, and the
pathological pattern; `test_parse.cpp` renders `case` as one string compare per construct, with
the empty arm and the arm whose last `;;` is spared; `test/run.mjs` has what neither can reach —
that `echo /bin/l*` names two real files, that `/home/g/.*` finds the dotfile that `/home/g/*`
must not, that a star out of a variable globs and a quoted one does not, and that `case` runs a
real body. The glob block sits late in `run.mjs` on purpose: every command spends pids, and the
`ps` cases above it assert a column width that a five-digit pid overruns.

---

## The tree learned to branch

Fourth of the stages in `src/sh/TODO.md`: `if`/`elif`/`else`/`fi`, `while`,
`until`, `for … in`, `break` and `continue`. `sh.wasm` went from 114,936 to 127,614 bytes and the
staging tree from 584,414 to 597,092 against an unchanged 1 MiB budget. **`kernel.wasm` did not
move and the syscall ABI did not change.**

A correction to the two entries below this one, since it is a measurement and not a judgement:
both were taken on a build tree still holding the `/bin/export` that S1 renamed to `save`, which
`copy_directory` never deletes. Clean, S2 ended at 584,414 rather than the 599,238 recorded
there. CLAUDE.md already warns that a release is cut from a clean tree; so, it turns out, is a
number.

**Almost nothing new had to be invented, which is the point of having built S2 first.** The node
arena took four kinds, `Flow` took two values, and `break`/`continue` went in as builtins through
the path `exit` already used — a flag the builtin sets, turned into a `Flow` by the pipeline it
ran in, consumed by the loop above. `builtin.h`'s rule needed no amendment: what they touch is
this process's own walk.

**`if` is a flat run of (condition, body) pairs where v7 nests.** The V7 shell desugars `elif`
into a nested `TIF` as the else-branch, which is elegant in a shell that unwinds with `longjmp`
and wrong in one whose walk is a chain of coroutine frames: `elif` nine times would be nine
frames deep. It is the same argument that made `Seq` and the `&&` chain n-ary in the stage
before, and it is now the third time it has come up, so it is worth naming — **in this executor,
depth costs frames, so anything that reads as a chain must be stored as one.**

**Three places where POSIX won over the reference, all deliberate.** An `if` whose condition
fails with no `else` reports 0; v7 reports the *condition's* status, because its `execute()`
zeroes `exitval` only for a non-null tree and the missing else-branch is a null one. That is a
bug every shell since has fixed, and scripts have assumed the fix for forty years. `for x in;
do … done` is legal with an empty list where v7's `chkword` refuses it. And `continue n` takes a
level, which v7 parses for `break` and ignores for `continue`.

One v7 behaviour was deliberately *not* reproduced: `break 9` inside one loop leaves its level
counter set after the outermost loop exits, and nothing there ever clears it, so the rest of the
script is silently skipped. `run_line` clears the request on the way out, so it can spill into
the rest of a line but never into the next one.

**A loop lets `Flow::Interrupt` through rather than consuming it**, and that one line is what
makes `while true; do sleep 5; done` come back on a single `^C` instead of one press per
iteration. `Break` and `Continue` are the only two a loop takes; `Exit` and `Interrupt` pass
through to `run_line`. The gap it leaves is now in CLAUDE.md: a loop whose body is *entirely*
builtins has nowhere for a `^C` to go, because the shell arms its children with `Sys::Fg` and is
never in its own foreground set. Every `while true` has a program in it today, since `true` is
`/bin/true`, so the gap only opens when S8 makes `:` and `test` builtins — which is the stage
that has to argue for them.

**A construct that runs nothing has to say so.** `exec_pipeline` is what publishes `$?`, so an
`if` that took no branch and a loop whose body never ran would have left the *condition's* status
visible even though they report 0. Both call `var_status` on that path and only that path — the
same shape the `!` operator already needed.

**`for`'s name and word list are a `Command`**, so the expander that serves a command serves them
too and `for i in $files` splits against `IFS` for free, with globbing to follow at S4 with
nothing added. The one catch was that `add_word` counts a leading `name=value` as an assignment
prefix, which would have hidden `for i in x=1` from `args()`; the fix is a defaulted parameter
that the `for` words pass false. Its items are copied before the loop starts, because a `set` or
`shift` in the body would otherwise move the vector being walked — v7 takes a reference on the
parameter block for exactly that reason.

**Reserved words are recognised by position and quoting is handled by doing nothing.** `ends_list`
is consulted only where a command may start, and `simple()` takes words greedily, so `echo done`
prints `done` and `while true do …` reads `do` as an argument to `true` — which is v7's behaviour
and why the `;` is mandatory. And because a word still carries its quotes at that point, `'do'`
is four bytes and can never match the two-byte keyword.

**Two things cost more time than the feature did.** A conditional expression choosing between
string literals — `w == "then" ? "…" : "…"` — is not a constant pointer, so `Str`'s
`__builtin_strlen` did not fold and the link failed on `strlen` with no libc to satisfy it. The
codebase already had that comment in `job.cpp`; it now has it twice. And a driver case that types
a 63-character command in one burst overflows the 64-slot key ring and loses its Enter, which
looks exactly like a hung shell. A human types slowly enough never to see it.

**What the tests prove separately.** `test_parse.cpp` renders the four constructs, so `if` with
an `elif` chain, a nested loop and a `for` with and without its `in` are each one string compare;
`test/run.mjs` has what the tree cannot show — that a loop rebinds its variable, that `break 2`
leaves both loops and nothing else, that `break` outside a loop is silent, and that
`if false; then echo x; fi` reports 0.

---

## A line became a tree

Third of the stages in `src/sh/TODO.md`, and the one it called the largest.
`;`, `&&`, `||`, `&` mid-line, `#` comments, `!`, `{ … ; }`, a newline as a separator, and a
half-typed construct that asks for another line under `PS2`. `sh.wasm` went from 104,292 to
114,936 bytes and the staging tree from 588,594 to 599,238 against an unchanged 1 MiB budget.
**`kernel.wasm` did not move and the syscall ABI did not change**; `smoke`'s exact import and
export surface stayed green untouched, which is what says so.

**`Pipeline` became `Tree`.** It already owned the store, the words and the commands; now it owns
the nodes above them, and a class named for one of the kinds it holds would have been read wrong
by every stage after this one. The rename is the whole of `parse.{h,cpp}`, `job.cpp` and
`test_parse.cpp`, and it is cheaper now than at any later point.

**The arena is index links, for the reason `freeze()` already gave.** `nodes_`, `kids_` and
`ops_` all reallocate while the parse grows, so a pointer into any of them dangles at the next
push. Indices also keep a frozen tree trivially movable, which `test_parse.cpp` asserts and which
a function body will depend on. `nodes_[0]` is always a `Nop`, so an index of 0 can mean
"nothing" without a sentinel of its own — an empty line is a tree whose root is 0.

**`&&` and `||` are one n-ary chain, not a left-leaning pair per link, and that is a *depth*
decision rather than a tidiness one.** `exec_node` recurses, so `And(And(And(a,b),c),d)` would
make `a && b && c && d` four frames deep and a hundred links a hundred frames. As one chain with
a byte of operator per link, the walk's depth is the *text's nesting* depth — braces, `!`, and
later `if`, `while` and a function — which is what the parser's bound of 16 actually bounds.
`Seq` is n-ary for the same reason, so a hundred-statement script is depth one.

The short circuit is a `continue` and not a `break`, and the difference is `false && a || b`: the
`&&` skips `a`, and the status carries down the chain so the `||` still reaches `b`. Breaking out
would have stopped the whole chain at the first failure, which is a different language.

**Control transfer is a field, because there are no exceptions.** `enum class Flow { Normal,
Exit, Interrupt }` on a `Ctx`, checked after every `co_await exec_node(…)` — one branch per node,
the same shape as `TRY()`. `Break`, `Continue` and `Return` join it when there is something to
break out of.

`exit` is the case that shows why a field and not a return value. It was already a builtin that
*asks* rather than acts, setting a flag the prompt loop reads at the end of a line; with a list,
`echo a; exit; echo b` would have run `echo b` before anyone looked. So `exec_pipeline` consumes
the flag where the builtin ran, turns it into `Flow::Exit`, and `run_line` re-arms it on the way
out. `shell.cpp`'s two loops did not change a line for it.

`Flow::Interrupt` was scheduled for the next stage, with the loops. It came here instead because
`;` is what makes it reachable: without it `sleep 5; echo done` echoes after a `^C`. A stage
reporting 130 stops the rest of the text, and `job.h` now says why that is the mechanism —
there is no signal, and a status is the only thing that crosses a process boundary. The price is
stated there too: a program that exits 130 of its own accord does the same.

**`Ctx` is its own heap block, and so is the tree.** `Run` held the `Pipeline` by value, and it
could not: the tree outlives every pipeline under it. `run_line` allocates the tree, `Run` borrows
it and shrank by half as a result. `Ctx` is thirty-odd bytes today and did not have to be on the
heap — but it grows with every stage left (break levels, traps, a scope, the `-e -x -u` flags),
and `run_line`'s frame is the root of a recursion. Putting it there now freezes that frame instead
of letting each later stage push it a little closer to the 512-byte cliff.

One `Run` per pipeline, `heap_new`'d and freed as each runs, rather than one cleared and reused:
`Vec` has no capacity-preserving `clear()`, ten of them would have to be got right every time,
and command substitution will want two `Run`s alive at once anyway.

**Multi-line input is re-parsing, and v7's best idea was rejected on its own merits.** The V7
shell serves `.`, `eval`, traps, `-c`, substitution and here-documents through one `fileblk`
chain — a single character source that can block. Here that would make the lexer a coroutine and
the recursive-descent parser a stack of them, five frames deep on every word against a 512-byte
budget, and it would drag `sys_async` into `parse.cpp`, which `tests.wasm` compiles precisely
because it touches nothing but `Str`, `String` and `Vec`.

So `parse()` gained a third outcome instead: `ParseErr{ message, more }`, where `more` means the
text ended *inside* something. Both readers accumulate and re-parse from the top — the prompt
under `Prompt{ {}, {}, ps2 }`, which `LineEditor` needed no change at all to draw, since a
`Prompt` was already three independent pieces; and `sh -s` over `LineReader`, one line at a time,
so `producer | sh -s` still streams rather than blocking to EOF. It is quadratic in the length of
one construct, and a construct is tens of lines against a pure parser that allocates into a store
it throws away. `more` is deliberately *not* a new `Error` value: that enum is kernel-wide.

`more` is set where the operator was — a trailing `|`, `&&` or `||`, an unclosed `{`, an
unterminated quote or `${` — and never by a command that simply had no words. `> f` is a syntax
error at once; `a &&` waits.

**A newline stopped being whitespace**, which is one asymmetry and no more: the lexer's leading
skip takes a new `is_blank` that excludes `\n`, while the word-terminating test keeps `is_space`,
which already included it. `#` starts a comment only where a token could start, so `echo a#b` is
still one word. `{`, `}` and `!` stay ordinary word bytes and are reserved by *position* — and
because a word still carries its quotes at that point, `'{'` is three bytes and can never match
the one-byte reserved word, so quoting is handled by doing nothing.

**Two refusals, both for the same reason.** `a && b &` and `{ … ; } &` are `cannot run a list in
the background`: backgrounding means the shell keeps running while the rest goes, and nothing in
a process can wait for a sibling task, so there would be nobody left to run `b`. What may be
backgrounded is one pipeline whose stages are all spawned children. And `{ a; } | wc` and
`{ a; } > f` are refused as *not yet* — a group needs the base stdio threaded through `Ctx`,
which is the redirection stage's work, and doing it twice is how the two would drift.

**`jobs` now lists a pipeline's own text.** The `JobEntry` copied the whole line, which was the
same thing when a line was one pipeline; with `echo hi; sleep 5 &` it would have filed the lot.
Each `Pipe` node carries the byte span it came from, which is two of the four fields a node has
spare, and the `&` falls outside it — so the listing reads `sleep 5`, which is what it always
should have said.

**What the tests prove separately.** `test_parse.cpp`'s `shape()` grew from a loop over stages to
a walk over the tree, and every construct is one string compare again: `{a};{b}`, `{a}&&{b}||{c}`,
`!{a}`, `{{a};{b}}`, and `?` rather than `!` in front of a message when the text merely ended
early. `test/run.mjs` has the four things the unit suite cannot see: that `false && a || b`
reaches `b`, that `$?` is the *pipeline's* and not the line's, that a trailing `&&` leaves `>` on
the screen and not a prompt, and that `echo before; exit 7; echo never` prints one of the two.

---

## The shell got variables

Second of the stages in `src/sh/TODO.md`. `$x`, `${x}`, the `${x-y}` family,
the specials, assignment as a word the parser knows, field splitting against `IFS`, and `set`,
`shift`, `unset`, `export` and `readonly`. `sh.wasm` went from 83,485 to 104,292 bytes and the
staging tree from 552,965 to 588,594 against an unchanged 1 MiB budget. **`kernel.wasm` did not
move and the syscall ABI did not change** — everything here is inside the process binary, and
`smoke`'s exact import and export surface stayed green untouched, which is what says so.

**`Field` needed a flag the mark could not carry.** The last stage put the quoting mark out of
band, one byte per byte, and that was right for everything the mark will ever be asked — except
one question it cannot be asked at all: *was quoting the reason this field exists?* With x empty,
`echo "$x"` must pass one empty argument and `echo $x` must pass none, and both are a zero-length
text beside a zero-length mark. V7 spends its lone `QESC` on exactly this. So `Field` gained a
`bool quoted`, set by the `'` and `"` arms whether or not they contribute a byte, and the
splitter keeps an empty field only when it is set. It was harmless while one field was always
pushed; the moment a word could expand to nothing it was the difference.

**Splitting happens during the walk, not after it.** We know whether a byte came from an unquoted
expansion at the moment we append it; after the fact we would have to consult the mark to find
out, and the mark would stop being scratch. So the splitter is one branch in `put_value`, and a
literal byte of the word never goes through it — which is why `IFS=:` leaves the typed word `a:b`
alone and splits `$path` in two. That distinction is a `bool literal` threaded through the walk,
and it also settles `${x-a b}`: the body of an operator is expansion output rather than the
word's own bytes, so it splits, where the same two characters typed directly would not.

**The expander took callbacks rather than calling the variable table.** `var.cpp` is pure enough
to have been linked into `tests.wasm` beside `expand.cpp` and called directly, and that would
have been less code. The seam is there for the two stages that need it: command substitution adds
a `substitute` callback beside the lookup, and a function body will want a table of its own that
is not the shell's. Four function pointers now is the shape those want, and the unit tests fill
them with table literals — which is also what keeps `expand.cpp` clear of every syscall, a fact
the link into `tests.wasm` enforces rather than promises.

**`x=1 cmd` sets, runs and restores — and a program cannot see it either way.** There is no
environment anywhere in the wasm ABI: `Sys::Spawn` hands a child three descriptors, an argv and a
cwd. So an assignment prefix on a program stage is expanded and then has nowhere to go. Rather
than leak it into the shell or refuse the idiom, the prefix is applied around the one stage that
can see it — a builtin, and later `read`, `eval` and a function — and the displaced values are
put back after. `x=1` with no command word is the ordinary case and persists. Per stage, so
`x=1 a | y=2 b` leaks nothing.

**`${x?}` aborts the line rather than the shell.** V7 ends a non-interactive shell there. Braam
prints `braam: x: parameter not set` and reports 1 in both modes, because script-entry policy
belongs to the stage that adds `sh file` and `sh -c` and there is nothing to be gained by
deciding it early. Expansion runs before anything is opened or spawned, so the line leaves
nothing behind.

**A command may now have no command word at all**, which two things make reachable: `x=1` alone,
and a word that expands to nothing. `run_line` indexed `args(i)[0]` unchecked at two sites,
correct only because the parser guaranteed a word and the expander guaranteed a field. Both
guarantees are gone. The null command performs its redirections, keeps its assignments and
reports 0, which is Bourne.

**`${` had to stop ending a word**, or `${x-a b}` would lex as two and `${x-a|b}` as three. The
lexer gained one arm, and it shares `brace_end` with the expander rather than carrying a copy —
unlike the quote arms, which are duplicated because each needed a different thing from the same
rule. `\$` also became escapable inside double quotes, in both, which changes no word boundary
but makes `echo "\$x"` mean what it says.

**`/bin/export` became `save`.** The builtin and the program wanted one name, and the builtin's
is not negotiable — it is in every script ever written. The program is the Blob download of
Concept.md §5.4 and its name never was: `import` and `save` are the pair now. The alternative was
shipping no `export` builtin at all, which is defensible only until §5.4's paragraph has to
explain why a standard name is missing.

**What the tests prove separately.** `test_expand.cpp` carries the whole `$` grammar against a
table-literal shell, including the empty quoted word against the empty unquoted one, `"$@"` with
no parameters leaving no field where `""` leaves an empty one, and the mark showing that an
unquoted expansion is *unmarked* — which is what will let a `*` out of a variable glob at S4.
`test_parse.cpp` renders an assignment prefix in brackets, so `[x=1]{ls}` is one string compare.
`test/run.mjs` has what the unit suite cannot reach: that a value set at the prompt survives to
the next line, that `echo a "$e" b` and `echo a $e b` differ by a space, and that `x=two echo hi`
leaves the shell's x where it was.

---

## The lexer stopped removing quotes

First of the stages in `src/sh/TODO.md`, which is the plan for making
`/bin/sh` a language rather than a prompt. Nothing a user can see changed: the grammar is the
same one line it was, and the same commands do the same things. What moved is *when* quotes come
off — out of `Lexer::next`, into a new `expand.cpp` that runs between the parse and the run.
`sh.wasm` went from 79,899 to 83,485 bytes, and the staging tree from 549,379 to 552,965 against
an unchanged 1 MiB budget.

**Two things force the move, and neither is a preference.** A loop body is parsed once and
expanded once per iteration, so `for f in *; do echo $f; done` cannot have its words collapsed at
parse time — there is nothing left to re-expand. And `$` acts inside `"…"` and does not inside
`'…'`, a distinction a one-pass collapse throws away before anything can act on it. Both bite in
the stages after this one, which is exactly why this one is separate: it is the whole refactor
with none of the new behaviour, so a regression here is visible on its own.

**`parse.h`'s comment was the thing that had to change.** It said words are owned "because quote
removal makes a word something the line does not contain" — and with quotes left in, a word *is*
a substring of the line again. The store did not become unnecessary, but its justification did:
it is now that a parse outlives the text it came from, which is what a function body and a
sourced file will need. The old reason was true and would have quietly stopped being true.

**The lexer got smaller and stopped allocating.** It already walked the quotes to find where a
word ends; it merely also copied. Now it only walks, and `next(Str &)` hands back a view. The
double-quote arm keeps its escape rule verbatim — only `\"` and `\\` — because that is not a
copying detail but a *scanning* one: without it a `\"` would close the run early and the word
would end in the wrong place.

**The quoting mark is a parallel byte array, and v7's encoding was rejected on the reference's
own argument.** The V7 shell marks a quoted character in bit `0200` of the character, and the
BESM-6 port had to move that to a `QESC` prefix byte because its console is UTF-8. Braam's is
too, and here it fails twice over: there is no `int`-wide character space to hide a flag in
either. So `Field` carries `String text` plus a `Vec<u8> mark`, one byte per byte. Nothing reads
it yet — field splitting and globbing are the only two readers there will ever be, and both are
stages away — but it is built now because adding it later means rewriting every branch of the
expander. It is deliberately unconditional rather than lazily allocated: an unused optimisation
nobody has measured is worse than an allocation nobody has measured.

**The expanded words are a second set, not a rewrite of the first.** `Run` grew `words`, `argv`,
`argv0` and `targets` beside the `Pipeline` it already held, because one raw word may yield any
number of fields and because the same tree may be run more than once. `Run::args(i)` replaces
`Pipeline::args(i)` at the four sites that build a spawn, find a builtin or name one in a
diagnostic. The views are built only after the last append, which is `freeze()`'s rule one level
up — though the reason differs: a `Vec<Field>` moves its elements by stealing their pointers, so
the bytes never move and the discipline is habit rather than necessity here. Habit is the point.

**What the tests now prove separately.** `test_tokenize.cpp` and `test_parse.cpp` render words
with their quotes still on and assert only where a word *ends*; the quote-removal cases they lost
are `test_expand.cpp`'s, along with the mark, rendered as `^` and `.` per byte so one string
compare checks text and mark together. `expand.cpp` joins `parse.cpp` and `tokenize.cpp` in
`tests.wasm` — it touches nothing but `Str`, `String` and `Vec`, and the link is what enforces
that. Two cases went into `test/run.mjs` as well, because the unit suite cannot prove the words
reach a real `argv`: `echo 'a  b'` and `echo a\ \ b`, both with **two** spaces, since one would
survive the word becoming two arguments and echo joining them back with a single space.

---

## Scrollback, and the renderer that was told nothing

`scroll()` memcpy'd the rows up and overwrote row 0, so everything that reached the top of the
window was gone. Shift+PageUp and Shift+PageDown now page back over it, half a screen a press,
the way the Linux console does. `kernel.wasm` went from 146,326 to 148,483 bytes against an
unchanged 256 KiB budget.

**The history has to be the kernel's**, and that is the one thing not open to argument. A row is
overwritten inside a `tick`, often many of them, and `host_present` fires once at the end — so by
the time the page could look, the row it would have kept is already gone. There is no capturing it
from `web/render.js` and no ABI that would let the page ask.

**The renderer, though, is told nothing at all.** `struct Screen` gained no field, there is no new
import or export, and `web/render.js` changed by one comment. The kernel composes the view into a
cell array and points `cells` at it; the renderer paints row *i* at canvas row *i* as it always
did. Selection came free out of that: `all()`, `text()` and `bounds()` index `S_CELLS` freshly on
every call, so a drag over scrollback selects and copies scrollback without a line of it knowing.
The alternative — `view_top` and `history_cells` in the descriptor, and `render.js` biasing its row
reads — would have had to re-teach the selection, `text()` and select-all what a row means, and
would have put the ring's layout into the ABI where the magic word is the only versioning there is.

**Which buffer moves is the whole design.** The obvious arrangement keeps `cells` pinned and
shadows the live screen while a view is up — and then every write has to be kept from damaging a
rectangle that is no longer on screen, which means hooks in `damage`, `damage_all`, `screen_touch`,
`screen_flush` and `screen_cursor`, plus a private path for the view's own repaint to bypass them
all. Six edits to the hottest code in the kernel. Inverting it costs one: `g_cells` stays the live
screen, every writer is untouched, and `cells` points at a composed block instead. A write while
scrolled back damages a rectangle the renderer then repaints out of the composed block, where those
cells are unchanged — wasted paint, never wrong pixels. What it costs is that the address in the
descriptor moves when a view opens or closes; nothing caches it, because `memory.grow` detaching
the buffer (§8.4) already forced everyone to re-derive, but a test that holds a descriptor across a
press is a hazard that did not exist before, so `test/run.mjs` says so where it reads `cells`.

**The live screen stays live, which is what keeps the anchor arithmetic honest.** Output goes on
into the grid underneath, `screen_scrolled()` goes on counting, and `LineEditor`'s anchor follows
it — none of them can tell there is a view. Freezing output instead would have meant deciding what
`Sys::Echo` should answer with, and the answer would have been a lie the moment the view came down.

**Composing is the flush's, not the scroll's.** `scroll()` runs once per output line. Recomposing
there would memcpy the whole grid that often — a megabyte a line at the widest geometry, thousands
of times inside one `tick`, which is a hang rather than a slowdown. It sets a flag instead, and
`screen_flush` composes once a tick, where `host_present` already is.

**The view stays on the rows being read.** When output scrolls the grid underneath, the offset
grows with it, so the text does not slide. Once the ring is full the oldest row is evicted and the
offset clamps instead, and from there the view drifts by a row per scroll — correct, since those
rows are genuinely gone, but the kind of thing someone later reads as a bug, so it is asserted.

**Any key comes home, and that is a choice.** The Linux console leaves you where you are until you
page back down. Coming home on the next keystroke is the more forgiving rule, and it also makes the
prompt provably live whenever anything is typed — so the line editor's anchor can never be pointing
into a view, and there is no interaction to reason about. Paging is therefore the only chord that
does not come home, and `MOD_SHIFT` was free: nothing in the kernel had ever looked at it.

**The pump owns the chord, but not while a program holds the screen.** `less` and `edit` page with
PageUp and switch on the code alone, and the grid they are painting is not the one with the history
behind it — so the claim gates the interception, and an unshifted PageUp was never the pump's
anyway. `FullScreen` brings the view home before it snapshots, and that is load-bearing rather than
tidy: `less /README &` claims the screen from a syscall with no keystroke in between, and without
it the renderer would sit on a frozen view while the pager painted underneath.

**512 rows, allocated on the first scroll.** A row is `cols * 8` bytes, so it is 320 KB at eighty
columns and 2 MiB at the widest grid the host can ask for; a screen that never fills pays nothing,
and a heap that will not give it up means no scrollback rather than no screen. A width change
re-widths the ring, clipping and padding each row exactly as the grid itself is clipped and
padded — there is no line model to re-wrap with, which is the same reason resize has never had one.
Only on a width change: `web/worker.js` calls `resize()` on every viewport event, and a rows-only
change must not copy megabytes per frame.

**`clear` does not clear it**, the way the Linux console does not. The screen blanks and what
scrolled off it is still there to page back to.

**Re-wrapping logical lines is still outstanding.** §3.5 promised it would "land with whichever
milestone needs scrollback"; scrollback landed without it, because the per-row continuation bit is
a property of how a row was *written*, and history stores rows that were already written. The
promise moves rather than being kept — and the selection that dies on the next keystroke is still
waiting on the same bit.

---

## /mnt/import became /import

The picker's landing directory sat under a `/mnt` that never held anything else. `/mnt` is where
Unix puts *mounted* filesystems, and §5.1's whole point is that there is one store and one
generated tree: `/bin`, `/share`, `/home`, `/tmp` and the import directory are all directories in
the same `OpfsFs`. A `/mnt` with a single ordinary directory under it promised a second
filesystem the system has no way to give, and cost every user of the escape hatch four extra
characters on a path they type by hand. `/import` says what it is at the root, beside the rest.

It is a rename with no mechanism behind it: `make_dirs` in `src/user/boot.cpp` creates one
directory instead of two, `import`'s default destination is `/import`, and the archive still
carries neither — `installOps` names only the top-level directories `rootfs.zip` actually
contains, so the tree it does not carry is still never touched.

**An existing store keeps its old `/mnt/import`, files and all.** Boot does not migrate it and
nothing removes it, because a boot-time rename would have to decide what to do about a collision
with a `/import` already made and would run on every boot for ever after. `mv /mnt/import/* /import`
then `rm -r /mnt` is the whole of it, and a store that is never touched simply carries a stale
directory — which is what §5.2 means by the archive, not the store, being what the system
recovers from.

---

## ls asks whether it is talking to a terminal

`ls` printed one name per line, always, and understood one flag. Bringing BSD's `ls` across —
`-C` in columns, `-R`, `-S`, `-r`, `-d`, `-h`, a `-l` that lines up — turned out to need one
thing the system did not have, and the rest fell out of it.

**There is no `isatty`, and §2.3 is why.** The terminal is a cell grid, not a byte stream, so
there is no escape sequence a program could send and no environment it could read a width out
of. `Sys::Cursor` reports the grid's geometry, but it reports it to a program whose stdout is a
pipe just as readily, so it cannot answer the question that matters. The kernel *can* answer it:
`stdio_console()` installs one sink and `pipe_sink`/`file_sink` install others, and that function
pointer is the whole of the difference. It was simply never reported. `Sys::Tty` is the operation
that reports it — the thirty-seventh, arg = fd, data = a flags word and the geometry.

Three choices inside it are worth the ink:

- **An explicit predicate in `tty.h`, not `ctx == nullptr` in the dispatcher.** The console sink is
  today the only `Stream` with a null context, so the implicit test would work. It would also
  silently promote the next sink that happens to need no context into a terminal.
  `tty_is_console` says what is meant, in the file that owns the fact; `console_is_input` is its
  twin for fd 0, so `arg = fd` is honest across its whole domain rather than over two thirds of it.
- **Zero geometry for a pipe, not 80.** A caller cannot distinguish an invented width from a
  measured one, and would believe it. The fallback belongs in the program, where it is visible:
  `ls -C` into a pipe formats at eighty because `ls` chose eighty.
- **A flags word rather than a bool.** It is what `SYS_STORE_*` bought `Storage` — room for a
  second fact about a terminal without a thirty-eighth operation.

`PROC_ABI` went 8 → 9. The change is purely additive and an old binary would never call the new
opcode, so nothing would actually break; the bump is the tree's standing rule that the number
tracks the table, and it buys a sentence at exec (`built for another process ABI`) instead of a
stale binary meeting `Err(Unsupported)` somewhere further in. In-tree it costs a rebuild, since
`tools/stamp.py` reads the constant.

**The flag parser is a third header in `src/proc/`, and a translation unit of its own.** `ls` went
from one flag to eight, and every command in `src/cmd/` was hand-rolling a leading-flag loop that
did not accept `-lR`. `proc/opt.h` is not part of `io.h`, which is one wrapper per syscall, nor of
`rt.h`, which is `_start` and `SysCall`. Being its own `.cpp` is load-bearing: `--gc-sections`
never extracts an unreferenced archive member, so the thirty-three binaries that do not call it
pay nothing — the rule the builtin table exists for, used the other way round. `ls` is the only
adopter here; `head` and `tail` would take `Opts{"", "n"}` unchanged, and were left alone.

**Column-major, and a shared column width.** A sorted listing is read *down* a column: filling
across would put `ls` and `mkdir` in different columns of the same row and make the sort useless
to the eye. The width is one number for the whole block, as BSD's is rather than GNU's per-column
fitting — with a `total` in 512-byte blocks under `-l`, `FS_BLOCK` being the system's own unit.
The last cell of a row is deliberately not padded: trailing blanks would leave the cursor in the
deferred-wrap column, and the test harness trims them, so that bug would have passed the suite and
shown up only in a browser.

**Width is codepoints, not bytes.** `screen_write` puts one codepoint in one cell, so
`Buf::put_left`/`put_right` — which pad by `Str::size()` — cannot be used on a filename at all;
`ls` counts with `utf8_decode` and pads a `String` itself. They are still right for the `-l` size
column, which is ASCII digits.

**`-R` is an explicit stack, not a recursive coroutine.** §8.2 prices a coroutine frame at a whole
size class, and a frame per directory level is the wrong shape for a tree. One heap `Vec<String>`
holds what is still to list; children are pushed in reverse of the order they printed, so they pop
in it, which is BSD's depth-first pre-order for free. Memory is bounded by sibling counts along
the current path rather than by the tree, and there are no cycles to guard against — this VFS has
no symlinks. The entry vector, that stack, the operand list and the row buffer are one heap block
for §8.2's reason, the same one `less` has a `Pager` for.

**What could not be brought across at all**: `-t`, `-i`, `-s`, `-o`, `-T` and `-L`. The filesystem
stores `{kind, size}` and nothing else — no mtime, mode, owner, link count or inode anywhere in
`Stat`, `Entry` or the `Sys::List` wire format. Any of those is a change reaching both storage
backends and the ABI, not a change to `ls`. `-a`/`-A` were left out for a different reason:
nothing in the tree creates a dotfile and nothing hides one, so the flag would introduce the
concept of a hidden file rather than expose it. `-F` is already there and unconditional — the
trailing slash on a directory is the only thing distinguishing it from a file in the short form,
and under columns it earns its place twice over.

**vmstat's rate columns went from five wide to six.** `W_RATE = 5` gave a five-digit rate no
separator from the column on its left, so `sy` and `cs` ran together into one number. The listing
work tipped `cs` over 10,000 and exposed it; the row is 74 columns now and still inside eighty.

The layout tests moved off `/bin` and onto a `/home/t` the suite builds itself. A listing of `/bin`
is a test that fails whenever a program is added, which makes it a test of the wrong thing; what
is left pointing at `/bin` asks only that `cd` is absent and `timeout` present.

`ls` costs 26,504 bytes against 15,633, and the staged tree 549,403 against 538,499 — 52% of its
budget. `kernel.wasm` grew 318 bytes for the new arm, to 146,342. Every `ls` now pays one extra
round trip for `Sys::Tty`, 34–45 µs; against it, a directory listing to the grid is one write per
*row* rather than one per entry, which on `/bin` is six writes instead of thirty-four.

---

## exec.cpp was four things in one file

At 2,211 lines it was a 3.4× outlier over the next largest source in the tree and more than half
of `src/user/`, and the reason it could not shrink was structural: everything in it sat in one
anonymous namespace, so nothing could leave without first becoming a name another translation
unit can see. `src/user/proctab.h` is that step — the process record, the descriptor behind a
number, the parked call, and the counted references over all three. It is private to the
directory in the only way this tree has ever made a header private: it is not installed, and it
says at the top that `exec.cpp`, `syscall.cpp` and `proctab.cpp` are the whole of its audience.
`exec.h` remains the surface for everyone else, which is why `boot.cpp`, `procfs.cpp` and
`main.cpp` did not change a line.

The cut is by *what the code is doing*, not by size. `exec.cpp` keeps the life of a process —
resolving a name, waiting for a worker, stepping the instance, and the four entry points the
kernel exports. `syscall.cpp` keeps what a process asks for: `proc_syscall` and the child a
`Sys::Spawn` creates, since `Spawned` exists only because that operation does. `proctab.cpp`
keeps the table and its accessors, and `exec_proc_state` with them — `/proc`'s view of a process
is a query over that table and nothing else, which is what lets `g_procs` stay a file-local of
the one file that owns it.

**The syscall switch stayed one file.** The `Sys` enum blocks the ABI into families and the
switch follows it, so cutting it four more ways along those lines was the obvious next move and
was decided against: a family would then be a coroutine of its own, and a coroutine is a frame
per outstanding call. The read of §8.2 that matters here is that a frame past 512 bytes costs a
whole 64 KiB span, and the dispatcher's frame is already the union of every arm's live state —
splitting it would have added a second frame to that, not replaced it. One file of 1,320 lines
with four visible sections is the cheaper shape.

**The helper set stops where the frame does.** `CO_CALL` (a Task that would not allocate is
`Err(NoMemory)`, not a crash) and `CO_RETRY` (an `Again` is a stray wake) are macros for exactly
that reason: a function wrapping an `await` is a coroutine, and these had to stay inline. What is
left is plain functions — `chunk_reply` and `write_status` for the tails a read and a write
share, `handle_bind` for the five arms that open something, `reply_u32` for the twelve that
answered with a fixed header. Nothing was factored that had fewer than three callers.

**Cancellation folded to one exit.** The switch had twenty-six `co_return Err(Cancelled)`
between its arms, each an early return out of a coroutine and each carrying its own unwind. They
are now a single check after the switch: `-Error::Cancelled` in `status` means the reply is
abandoned, wherever in the switch it was set. That is the same semantics — no arm ever reported
cancellation *to* the program — and it is where the size went: 147,182 bytes before the split,
146,024 after, against a budget of 262,144. A refactor that was supposed to cost a little
returned a kilobyte.

Two out-of-memory paths were wrong and are fixed in passing, both the same mistake. `Sys::Fetch`
bound its handle into the descriptor table and *then* released it if copying the headers failed,
leaving the table pointing at a freed block; the release now happens before the bind. `handle_bind`
exists so that the other four arms cannot make the same mistake: it binds, or it closes what it
was given and reports `-NoMemory`, and there is no order left for a caller to get wrong.

---

## The tree that becomes the root is called rootfs

`bundle/` is now `rootfs/`, and the staging copy `build/bundle/` is `build/rootfs/`. The name was
the last survivor of `bundle.bin`, the packed blob that `rootfs.zip` replaced: since then the
directory, the staging tree, the size budget's entry and the archive the host fetches were one
thing under two names, and the one nobody sees on disk was the one the code used. The budget line
reads `rootfs/` and the archive beside it `rootfs.zip`, which is the point — the tree and the file
are the same content, compressed or not.

`/bin/version` went at the same time. It printed `BRAAM_VERSION` and nothing else, which the boot
banner, `/proc/version` and `uname -a` each already say; a whole binary, its worker and its
instantiation for a string the kernel prints unasked. It was also the only program that included
`kernel/version.h`, so the `revision` dependency the build named one target at a time lost an
entry rather than gaining a rule.

---

## What the kernel is doing, as rates

`ps` says what each task *is*. Nothing said what the system was *doing*. Every counter the kernel
kept was a running total on `/proc/meminfo` — `allocs` and `frees` — and nobody subtracted them, so
the figure §8.2 calls the primary workload of the whole system was invisible as a rate. The costs
§4.4 quantifies were in the same position: two `postMessage` hops per syscall, one syscall per
512-byte chunk, asserted in prose and impossible to watch. `vmstat 1000` during `cat big | wc -c` is
the first thing that shows them.

**One file, because a row is one moment.** The counters could have been columns added to
`/proc/meminfo` and gauges added to `/proc/tasks`, and then a row would have been two `open`s and two
moments — which is exactly the mistake `/proc/tasks` was introduced to avoid. `/proc/stat` therefore
repeats the heap's figures rather than referring to them, the same duplication `/proc/tasks` has
against `/proc/<pid>`, and for the same reason. Both branches call `heap_stats()` once so neither can
drift.

**`now` is the first line, and it is the whole design.** It is the `sched_now()` the counters below
it were incremented against, so the reader divides by the file's own elapsed time rather than by how
long it asked to sleep. A throttled background tab, a late timer and a `^C`-shortened sleep all come
out right, and the interval argument degrades to a fallback divisor for the case — reachable, since
the clock is truncated to whole milliseconds — where two reads land inside the same millisecond. It
also makes the since-boot first row fall out for free: `counter / now` is BSD's `rate()` over
`getuptime()`, with no special case.

**A woken token is two different events and is counted as two.** `sched_wake` has four callers and
only one of them is the host: a channel wakes its peer, and an exiting child wakes a parked parent.
Counting them in one column would have put every byte moved through a pipe in the same figure as an
answer from outside, so `wc a | grep b` would have read as an interrupt storm. `Waiter::parked` — the
flag `/proc` already uses to tell a `park` from a `host` wait — splits them into `wakes` and
`unparks`, which costs one branch. `unparks` is a number nothing else in the system exposes: pipe
traffic has never been observable.

**`frames` is exact rather than a share of `allocs`.** A `Task`'s promise declares
`get_return_object_on_allocation_failure`, so its frame is allocated through the nothrow `operator
new` and nothing else in the tree reaches that overload. One increment there is precisely the
coroutine-frame count, which is the number §8.2 is about; `allocs` mixes it with every string and
every heap record. `fails` is on the same principle: every error in the system funnels into
`Err(NoMemory)` and this is the only place the count becomes visible.

**`ready` is a task suspended on nothing, not the depth of the ready queue.** The tempting
definition, `ready.size() - head`, is wrong here for a reason that is easy to miss: `/proc/stat` is
generated inside `open`, inside a syscall server, inside the drain loop, so that figure would measure
whatever happened to be queued behind the reader in one drain — and would exclude the reader. The
definition used is the one `/proc/tasks` already prints as state, which makes `ready + on_timer +
on_host + on_park == tasks` an invariant a test can assert, and makes `r` mean what `ps` prints as
`R`. The consequence is that `vmstat` is always in its own row — `r` and `p` are never zero while it
is the one asking, since its syscall server is runnable and its stepper is parked on the reply. BSD
subtracted itself out (`total.t_rq - 1`); that would be wrong here, because `cat /proc/stat` reads
the same file and is not the same shape.

**Counters live in `Sched`, gauges are computed there.** A file-static would survive `sched_reset()`
and accumulate across the fixed-order unit suite; a member of the object the reset destroys is zeroed
with it, which is what a test starting from a clean scheduler means. The heap's counters are the
other way round — `heap_init` runs once — so `/proc/stat` mixes two zero points. That is invisible in
a running system, which has one `Sched` lifetime, and only shows up in the test suite.

**No CPU column, and the group that replaced it says so.** Kernel-busy milliseconds are measurable
with two `host_now()` calls around the tick drain, and were rejected: in a browser tab the answer is
zero almost always, and as a percentage of wall time it would read 99–100% idle — a column bought
with a hot-path cost that says nothing, and a label reading `cpu` would read as retiring a documented
gap (§4.2). `-loop-` is in its place: how often the host granted the event loop a turn, which does
vary, and collapses when the tab is backgrounded. `-alloc-` similarly stands where BSD's `-page-`
was, there being no paging; allocations are this system's page faults.

**Seconds, which makes it the one time argument here that is not milliseconds.** `sleep`, `watch -n`
and `timeout` all take milliseconds, and consistency with them was the first choice; BSD's units are
the better one. The columns are rates *per second*, so the interval that divides them wants the same
unit, and `vmstat 1` has to mean the useful thing rather than an interval that would measure almost
nothing but vmstat itself. Memory is in raw KB rather than `ps`'s scaled units for a related reason:
a `vmstat` column is scanned downwards, and a unit that changes between rows defeats the only thing
that makes scanning it worth doing. BSD's `pgtok` does the same.

**What is deliberately absent.** `-i` would need every wake token tagged with a source, and a token
is opaque on purpose. `-m` is the interesting one — `domem()` prints kernel malloc by bucket size and
the allocator has exactly that shape, `SIZE_CLASS[]` plus whole-span blocks — but it needs per-class
counters in the allocator's hot path, and it is a separate argument to make. `bi`/`bo` were dropped
because `sy` already exposes that traffic: bulk I/O is one syscall per `SYS_CHUNK`. `-f`, `-t`, `-M`
and `-N` have nothing to report: no fork, no page-in timing, no core file, no namelist.

**The file is emitted a line at a time, and that is not a style choice.** `Buf` truncates silently
and `overflowed()` is called nowhere in the tree, so a single buffer for twenty-three lines would
have been a latent bug that returned the moment someone added a counter — and because `stat` and
`read` both work off the same snapshot, they would have agreed on the short answer and no reader could
have told. A per-line `Buf<32>` holds the worst case, eight padded characters and a space and twenty
digits and a newline, and makes the file's total length unbounded. The counter table is then the one
place a counter is added, which is also why `vmstat -s` prints an unknown name under itself rather
than needing an edit.

**`vmstat` measures itself**, about four syscalls per row: noise at a one-second interval, and most
of the reading at 100 ms. BSD has the same property and does not correct for it either.

One test needed narrowing rather than fixing: `ls /bin | grep ta` asserted the match was exactly
`tail`, and `vmstat` contains `ta`. It is `grep tai` now.

## MEM is measured, and it rides back on the step

`ps` reported the memory *cap*, and every row read `16M` — one number, the same for every process,
which is a column that costs eleven characters and says nothing. It reports what the instance has
actually committed now.

The figure exists after all. The section below is right that nothing will report a **worker's**
memory — that needs `measureUserAgentSpecificMemory()` and therefore COOP/COEP — but a process is
not only a worker: it is a `WebAssembly.Memory`, and a Memory says its own size.
`buffer.byteLength` is the pages the instance has grown to, read off the object rather than
measured, and it is exactly the RSS analogue `ps` wanted. Only the worker holds it, so only the
worker can read it — which is why `Memory.pages()` is in `web/host.js` beside the other accessor
that has to re-derive after a `grow`.

**It comes back on the step, not on an operation of its own.** A new `SvcOp` was the obvious shape
and is the wrong one twice over: `/proc` is generated at `open` with nothing to await (§5.1), so an
asynchronous query could not have fed the file it exists for; and a step already carries a message
each way. `result_hi` in the reply record was unused by `ProcStep`, so the pages travel there and
`proc_step` takes a `u32 *pages` out-param the way `exec_process` takes `bool *died`. No new
operation, no extra round trip, and nothing at all on the hot path — the kernel reads the field it
is already being handed.

So the figure is as of the process's last step, and that is the most current one there can be: a
process grows its memory only while it runs, and it runs only during a step. A process that has
not finished its first step reports `ProcMeta.initial_pages`, which is what the host sized the
Memory to — the truth then, rather than nought.

`/proc/<pid>` carries both, `mem` and `cap`, since a per-process ceiling is worth having somewhere
even when it is uniform; `/proc/tasks` carries both as fields and `ps` prints only the first. The
column scales like `df`'s, one decimal below ten, because the numbers now vary: the shell reads
`1.2M` and `cat` `448K` where the cap column only ever said `16M` for both. `ps` asserts in the
suite that what init holds is neither nought nor the whole cap, which is what proves a host figure
arrived rather than a placeholder.

So the shell holds 1,310,720 bytes of its 16 MB and `cat` 458,752, and the ceiling was never the
thing worth watching — the difference between two programs is.

---

## ps, and what a browser will say about a worker

The question that started this was whether the browser lets us look inside a worker, so that `ps`
could report what each one is doing. It does not, and the answer is worth writing down because it
looks like an oversight and is not one:

- There is no enumeration. No `navigator.workers`, no registry, no parent-side list. The only
  handle to a worker is the `Worker` object that created it, which is why `web/proc.js` keeps
  `procs` and `idle` itself.
- The handle is opaque: `postMessage`, `terminate`, `onmessage`, `onerror`. No id, no state, no
  start time, no exit code. `onerror` is the only involuntary signal, which is why `broke()` has
  to synthesise `STEP.TRAPPED`.
- There is no per-worker CPU time anywhere in the platform.
- The one memory API, `performance.measureUserAgentSpecificMemory()`, requires
  `crossOriginIsolated` — the COOP/COEP headers this system deliberately does without (§C.3), so
  that it hosts anywhere. Even where it is available it reports an agent-cluster breakdown, not a
  worker. `performance.memory` is non-standard and main-thread only.

DevTools sees workers through the CDP, which is not reachable from a page. So *nothing* a browser
offers would have helped, and none of it was needed: **the kernel already keeps a better table
than the platform could have given us.** A worker is exactly a process, so "does this task hold a
worker" is `proc_find(pid)` succeeding, and `ps` costs no host round trip, no new import and no
ABI change at all.

### /proc/tasks, because a row per read describes a row per moment

`ps` reads one file. The alternative — list `/proc` and read `/proc/<pid>` per row — costs two
`postMessage` hops per syscall *and* breaks the rule the same §5.1 paragraph states: content is
produced at `open` so that a read cannot describe two moments. Twelve rows read one at a time are
twelve snapshots. `/proc/tasks` is generated in a single pass, so the table is coherent.

Its fields are positional like `/proc/mounts`, with the cwd last so nothing after it needs
finding: a path may hold a space, and the shell does quote. A name may too, so a space in one
becomes an underscore — a name is a display field, and the alternative was a quoting rule in a
file two programs parse.

`/proc/<pid>` grew the same facts as named lines and its value column widened from six to seven
to fit `worker`. That is a visible change to a file people `cat`, and the cheaper alternative was
a cryptic four-letter name for the one fact `ps` puts at the centre of its table.

### The columns, and the two that are honest about what they cannot say

`PID PPID NAME STAT WORKER WAIT CALLS FDS MEM ELAPSED CWD`. STAT is BSD's: a state letter — `R`
ready, `S` waiting, `C` cancelled — then `+` for the foreground and `k`/`s` for the console
claims. So STAT says whether a task is suspended and WAIT says on what, with no redundancy.

**MEM started as the cap, not the usage** — `ProcMeta.max_pages`, which `spawn_process` had been
computing and discarding — on the reasoning that no browser API reports what a worker holds. That
is true of a *worker* and not of an instance, and the section below corrects it: MEM is the
measured figure now. **There is no CPU column**: nothing meters one, and a runaway program is
killed rather than measured.

### WAIT needed a bit in the Waiter, because a token cannot say what it is for

WAIT was to come free from the queue an awaiter registers in — the timer queue or the wake table.
It does not. `Channel` parks with `sched_wait_token` and so does a host call, and the stdio path
parks a token of its own through `park_receiver`. Every blocked task read `host`, including a
stage blocked on a pipe, which is the one distinction the column exists to draw: is this pipeline
waiting for data, or is the host wedged?

So the awaiter says so. `Waiter::parked` is set by `Channel`'s two awaiters and by `Stream::Write`
and `Source::Read`, where a non-null `park_` *is* the fact that this is a channel. Four bytes per
suspended frame against a column that would otherwise have lied. A `sleep 5000 | wc` now reads
`timer` for the sleeper's server and `park` for the one blocked on the pipe.

### A syscall server is a task, and names the process it serves

Every outstanding syscall is its own scheduler job (§4.3), so they appear in `ps` — a process
named `wc` beside its server named `/bin/wc`. Left alone they look like orphans, so
`exec_proc_state` searches the `Call` records for one whose `server` is this pid and reports its
owner as PPID. It holds only while the call is outstanding: once answered, the record is erased
and what is left is a coroutine finishing, which is what `/proc` then says about it. Recording
the owner on the job instead would have contradicted §3.3 — a job is a task, a name and a
`CancelToken`, and that is all the scheduler keeps.

`ps` is wide, so the suite widens the grid for it the way it already does for `help`.

---

## df is a table

`df` printed a four-line key/value block — `backend`, `mode`, `quota`, `used` — and then one
prose line per mount. Every fact §5.3 asked for was in it and none of it looked like `df`. It is
the BSD table now:

```
Filesystem  1K-blocks     Used    Avail Capacity  Mounted on
opfs         10485760      487 10485273     0%    /
procfs              0        0        0      -    /proc
```

**Nothing was added to the ABI, because nothing had to be.** The two sources `df` already used
carry the whole table: `/proc/mounts` names each mount and its filesystem, and `Sys::Storage`
answers with the live quota and usage. A per-mount `statfs` operation was the obvious shape and
is the wrong one — there is one store behind every writable mount, so the syscall would return
the same pair however many times it was asked, and Concept.md §4.3's table would have grown an
operation to say so. What the row needs from the kernel is which mount is backed by the store,
and the `kind` field has been saying that since M5.

So a mount whose kind is `opfs` reports the origin's figures, and two of them would report the
same ones. That is not a fudge: they *are* one store, and BSD shows one device mounted twice the
same way. Anything else answers from `Fs::bytes()`, which is still declared and still
unimplemented — `/proc` holds no bytes and says `0 0 0 -`, which is the honest row rather than a
missing one.

**The mode moved to the boot banner, and that amends a criterion.** M5's second was "`df` reports
quota, usage, and persistent versus best-effort mode". The quota and the usage are the table.
Durability is not a per-mount fact and had nowhere to sit in one: it belongs to the origin,
beside the quota, on the `store:` line boot already prints. The backend went with it for a
simpler reason — `boot_filesystem` refuses to start without OPFS and sync handles, so `backend
opfs` was a line that could never read anything else.

The cost is real and is accepted rather than worked around. `persisted` is not settled when boot
reads it: the page sends a provisional best-effort answer after a 250 ms grace period and the
real one when the browser decides, and the late one corrects `OpfsStore.persisted` in JS. `df`
asking again was what made that correction visible; a banner line is a snapshot, so a store that
becomes persistent a second after boot goes on saying `best-effort` until the next reload.
Putting the mode back in `df` to recover that would put a column in the table that is the same in
every row, which is the thing the table was written to stop.

**`-h`, because the number is gigabytes.** `10485760` is not a figure anyone reads; `10G` is. The
scaling is a dozen lines in `df.cpp` rather than anything shared — `text.h` has no formatter and
nothing else wants one. It keeps a decimal only below ten, so three significant digits carry the
magnitude: `9.9G`, `487K`, `0B`.

**Alignment is `Buf`'s now.** Nothing in the tree could right-justify a number: `ls` does not
align at all, `date` hand-rolls a two-digit pad, `/proc` pads its keys with literal spaces in the
string. `put_left`, `put_right(Str)` and `put_right(u64)` are three members on `Buf`, which is a
template — the kernel links none of them. A value wider than its column is written whole and
pushes the rest of the row, because a truncated size is worse than a ragged line.

The widths are BSD's exactly, including the quirk where the capacity column's header ends two
places past its values. That puts the whole row inside sixty columns, which matters here more
than it does on a terminal: the test grid is sixty wide and wrapping would have made every
assertion a two-line one.

---

## The banner names the host, not the heap

The boot line read `braam 0.2.65-6e62a37 — heap at 0x00030000, 64 KiB, 1 allocs, up in 200 us`.
Three of its four facts were allocator internals that `/proc/meminfo` already reports in more
detail, printed at the one moment nobody is asking about the allocator. What was missing is the
thing a browser makes genuinely hard to know: what the system is running *on*. Nothing in `web/`
read `navigator.userAgent`, `hardwareConcurrency`, `deviceMemory` or `userAgentData` at all.

Now the version line carries the version and the boot time, and a block under it names the
browser, the OS, the architecture, the cores, the memory and the store. The same facts are
`/proc/host`, and `uname` reformats that file the way `mount` reformats `/proc/mounts`.

The grid's own geometry is in `/proc/host` and not in the banner: it is a fact about the screen
it would be printed on, so it is the one line a reader can already see the answer to. `/proc/host`
reads it fresh on every open, which is what makes it worth having there — it moves with the
window, and a boot-time number would be wrong by the first resize.

**Two constraints decided the shape, and neither was negotiable.** `init()` is a synchronous
export, so nothing in it can `co_await` a service — and it runs before the host's first
`resize()`, so `screen().cols` there is always the placeholder 80. And `/proc` is generated
synchronously: `generate()` returns `bool` and `stat`/`list`/`open` all call it without awaiting,
so a `/proc/host` that asked the host on every read was never possible. Both point one way: ask
once from the boot coroutine, which runs on the first tick with the real geometry in place, and
keep the answer. A cache is not a compromise here — a browser does not change under a running
tab. The storage figures cost nothing extra either, because `boot_filesystem` already awaits
`storage_info()` as its first act and has the quota in hand.

**What a browser will not tell you.** There is no `navigator.cpu`. A CPU model and a clock rate
do not exist in any browser API, and the two ways of faking them were both refused: a
microbenchmark measures the JIT and the coarsened timer as much as the silicon, and the
WebGL renderer string names a GPU, not a processor. They are omitted. Cores come from
`hardwareConcurrency`, which is everywhere; architecture, OS version and full browser version
come from `userAgentData.getHighEntropyValues()`, which is Chromium's alone.

**No user-agent parsing, deliberately.** It would have filled the gap on Safari and Firefox, and
it would have lied: Safari's UA still says `Intel Mac OS X 10_15_7` on Apple Silicon. A field
nothing can answer is left out, and the raw agent string is shown instead of an interpretation of
it. That is the whole reason the wire format has a blank line in the middle — above it is what is
short enough and useful enough for the boot grid, below it the locale and the agent string, which
wraps a row on its own and only carries information when the interpreted fields are absent. Boot
writes the half above; `/proc/host` serves all of it. It also means the kernel never parses a
field: `next_line`/`next_field` live in `src/proc/io.h`, the process runtime, which the kernel
cannot reach — so the split is a single scan for `"\n\n"` rather than a parser in `boot.cpp`.

**Two things `userAgentData` gets wrong if taken literally.** Chrome's `brands` lists both
`Chromium` and `Google Chrome`, and varies the order on purpose to break sniffers — so taking the
first non-GREASE entry names a different browser on different loads. The specific brand is the
one worth printing, so `Chromium` is chosen only when nothing else is there. And Windows reports
a `platformVersion` that is not a Windows version: Windows 11 says `15.0.0`. That mapping is
published rather than inferred, so it is applied; UA-CH answers `0` for 7, 8 and 8.1 without
distinguishing them, and those get no number at all rather than a wrong one.

**The formatting is the host's.** `describeHost()` in `web/svc.js` returns finished `name value`
lines, the value at column 9. The colon the boot banner shows is added as it writes, by
`write_labelled` in `boot.cpp`: it is presentation, and putting it in the stored text would make
`/proc/host` a worse table — `uname` reads a field out of that file with `next_field`, and the
name would carry punctuation. It is also why `timezone`, the one name that fills the column, gets
its separating space from the writer rather than from the padding. Which fields exist varies by browser, so composing them in the kernel would have meant
teaching it about `userAgentData`; this way `kernel.wasm` grew by the price of one service
wrapper. `HostInfo` sits with the other services rather than after the process operations, which
renumbered `PROC_SPAWN`/`STEP`/`KILL` — safe because only `web/svc.js` writes those numbers down,
the C++ side using enum names and `test/fakesvc.mjs` importing the table.

**`uname -a` prints the block rather than one packed line.** There is no POSIX contract here, and
four fields is not what is worth reading about a browser. `-s`, `-r` and `-m` are the classic
three, and `-m` is `wasm32` — the one fact neither the host nor the version supplies.

Storage stayed out of `/proc/host`. Quota and usage are live, `df` exists to ask for them, and a
cached copy would go stale. The banner names the quota alone for the same reason: the usage is the
half that moves, so a figure true only at boot is the wrong one to leave on the screen all
session, while how much room the origin gave is a fact about the machine like the others around it.

The banner block costs three rows of the grid. On the 60×16 screen `test/run.mjs` resizes to, boot
occupies 13 rows of 16 — which is why the fake's host string in `test/fakesvc.mjs` is two short
lines, and why the motd is seven. A longer one would push the greeting off the top, and the motd
assertion is what would catch it.

---

## One store, and rootfs.zip

Three filesystems held the namespace: `MemFs` for `/`, `BundleFs` for a read-only `/bin` and
`/share` unpacked from a fetched archive in linear memory, `OpfsFs` for `/home`. Only `/home`
survived a reload, every boot downloaded 491 KB whether or not anything had changed, and the
files a user made outside `/home` — in `/tmp`, in `/mnt/import` — quietly did not exist after
one. Now `/` is one `OpfsFs` and everything else is a directory in it. `kernel.wasm` lost 10 KB
with the other two filesystems, but that is not why: three stores meant three answers to every
question about durability, and users only ever wanted one.

**Nothing above the VFS had to change.** `exec_resolve` reads a program image with `read_file`
and boot builds its directories with `vfs_mkdir`, so which backend a path lands in was never
visible from userland. That is the property the mount layering was built for, and this is the
first change that spent it.

**The unpacker moved to JS, and the format became a zip.** `BBND` existed because a kernel had
to parse it: 231 lines of `bundlefs.cpp` reading a table of offsets into a blob it kept in
linear memory, with a matching writer in `tools/pack.py`. Once the archive is written into OPFS
rather than mounted, the only thing that reads it is `web/fs.js` — and there the browser's own
`DecompressionStream` and Python's `zipfile` do the work, so the hand-rolled format bought
nothing and cost two implementations to keep in step. `rootfs.zip` is inspectable with
`unzip -l`, buildable by hand, and 204 KB where `bundle.bin` was 491 KB.

Deflating it moved what the size budget means. That entry exists so §4.4's per-binary
duplication "shows in one number", and a compressor hides precisely that — so `size_budget.py`
grew a directory branch and the budget now names the *staging tree*, at the number `bundle.bin`
used to report. The download got smaller as a side effect rather than as a way of not looking at
the total.

**The fetch is lazy, and the stamp is what makes it so.** The host writes `/version` as the last
step of an unpack, with the string the kernel passed — its own `BRAAM_VERSION`, so the stamp
cannot disagree with the kernel that asked for it. Boot compares, and a match means the archive
is never requested: the steady state costs no download at all. Written last, so an interrupted
unpack leaves a stamp that does not match and is done again rather than believed.

A mismatch asks. The alternative was overwriting on sight, which is what a service worker would
do, but a stored `/bin` is the user's — they may have put something there — and the prompt costs
one line above a prompt they were about to see anyway. Declining boots on what is stored; if
those binaries are stale, `exec_resolve` already says so by name ("built for another process
ABI"), so the failure explains itself rather than needing the boot path to predict it. The
prompt reads keys through `KeyInput`, which works because init spawns the console pump before
this task — the same claim a full-screen program takes, a few hundred milliseconds earlier than
anything else has ever taken it.

**`/bin` is writable now, and that needed an answer.** It used to be immutable by construction:
a read-only archive mount refused a write below the VFS. One store means one `writable()`, and
`rm /bin/sh` is reachable from the prompt — with a matching stamp, nothing would ever put it
back, and the origin could not boot again. So `no_shell` offers the unpack a second time. The
rule that comes out of it is worth stating plainly: **the archive, not the store, is what the
system can be recovered from**, which is also why it is never cached into the store as bytes.

A per-file read-only flag was considered and rejected. OPFS has no such thing — `FileSystemHandle`
carries `kind` and `name` and nothing else, and `createSyncAccessHandle({mode})` chooses a lock,
not a protection — so it would have to be Braam's own metadata, in a manifest or a sidecar, with
no inode layer to hang a bit on and nothing to keep a user from rewriting whatever held it. With
one user and no privilege boundary it would protect against accident only, which the restore
prompt already does.

**No OPFS is fatal, and that retires a criterion.** M5's third was "with OPFS unavailable, the
system boots on `MemFs` and says so", and it was a good criterion while `/home` was the only
thing at stake. It is not one now: there is no second store, so a memory namespace would be a
system that looks like it works and loses everything at the reload — the failure mode the line
was written to prevent, arrived at by a different road. Boot says what is wrong and starts no
shell. The criteria list keeps it, struck, the way M8's third is kept.

**What the tests lost, and where it went.** `test_memfs` and `test_bundlefs` are deleted with
their subjects. `test_vfs` needed a writable filesystem that answers *without suspending* —
`run_now` panics on a suspension and `OpfsFs` awaits the host for everything but a read — so it
grew a `TempFs` beside the `ReadOnlyFs` and `CountingFs` it already had. A fixture rather than a
component: flat, no `df` accounting, and honest about it in a comment.

`test/run.mjs` drives everything synchronously, and there is no synchronous inflate. Rather than
make the driver async — which would reach every assertion in it — `parseZip` is separated from
`installOps`: the parser is async and runs once during the driver's own setup, and the generator
of operations is plain and runs inside the import. Both stores drive the same generator, so
neither has its own idea of what an archive means. It also made the one check worth writing by
hand cheap: an entry named `../escape` is put in front of the parser and has to be refused.

---

## A release archive dated today

Every entry in `braam-<version>.zip` was stamped `01-01-1980 00:00`. That was deliberate — see
"0.1.0 — Packaging", below — and the reasoning stands: a zip timestamp is the only difference
between two packs of one tree, so fixing it is what let `md5` answer "is what is deployed what I
built?" without unpacking anything.

It is the wrong default anyway. The determinism is worth something to one person once in a while;
the date is read by everyone, every time, in the `unzip -l` that precedes a deployment, and in the
mtime of every file the deployer unpacks. 1980 is not merely uninformative there — it is *wrong*
in a way that reads as a broken build, and the file it lands on looks older than everything around
it for as long as it survives. A property worth checking on demand lost to a property being
misread continuously.

So the stamp is the time the pack ran, and `SOURCE_DATE_EPOCH` puts the old behaviour back exactly:
set it and two packs of one tree are identical again. That is the reproducible-builds convention
rather than a flag of ours, so whoever wants the guarantee already knows the name, and CI can pin
it without knowing anything about this script. Sorted entries and the fixed mode never moved, so
what changed is one field.

The two clocks are read differently on purpose. `SOURCE_DATE_EPOCH` is rendered in UTC, because a
stamp that moves with the packer's time zone is not pinned; the pack time is rendered local,
because a zip's date field is local time by definition and has no zone to carry — rendering
*that* in UTC would show a deployer in Los Angeles a build seven hours in their future.

The commit date was the third candidate and is the tempting one: deterministic, meaningful, and
already at hand since `version.py` shells out to git. It says when the *source* was written, which
is not what the reader of an archive is asking; a release cut a month after the last commit would
be dated a month early, and a tree with no repository — an unpacked release, which is a supported
way to build — has no such date at all, putting 1980 back for the case that has the least context
to interpret it.

## One handle per file, not one descriptor

`cat bar bar` said `cat: bar: permission denied`, and so did `wc /bin/sh /bin/sh`, `grep x f f`
and `grep one notes < notes`. The refusal came from the VFS open-file table, which scanned for the
absolute path and refused *any* second open, system-wide, on every backend — including `/bin` and
`/proc`, which have no lock to protect.

That rule was deliberate, and the reasoning behind it (see "One open handle per file", below) was
right about the thing that mattered: OPFS's `createSyncAccessHandle()` takes an exclusive lock, and
a rule that held only on some backends would be worse than the restriction. What was missed is
that there is a third option. The table now holds **one backend handle per file and hands out
references to it**. Nothing is ever asked to open a file twice, so the rule is identical on OPFS,
MemFs, BundleFs and ProcFs *without* being a restriction — the property the strict rule was
protecting, obtained by removing the strictness rather than by spreading it.

Sharing is safe only because of something that was already true: the offset lives in `FileIo`,
above the VFS, and `vfs_read`/`vfs_write` take an absolute one. "Nothing below it holds
per-descriptor state" was an observation; it is now a correctness precondition.

**Readers share, a writer keeps the file to itself.** That restores what §5.2 originally meant.
Two writers would need a shared offset the VFS deliberately does not have — `O_APPEND` is resolved
once, at open, into `Handle::file.off`, so `a >> f` and `b >> f` at the same time would overwrite
rather than append, and POSIX's atomicity is not available to us. Keeping `cat f > f` refused is a
feature the shell gets for free. `may_share()` is the one function to change if that is ever
revisited; it is also why `OpenShared::mutating` never needs recomputing, since a record that has
it can never gain a second descriptor.

`O_TRUNC` counts as writing even without `O_WRITE`. A share skips the backend open, and the
backend open is where a truncation happens, so an `O_READ|O_TRUNC` open that shared would silently
not truncate. `O_CREATE` needs no such care: if a record exists, so does the file.

The old invariant already leaked. `vfs_open` scanned before `co_await fs->open(...)` and installed
after, so two tasks could both pass the scan and both install; it only looked airtight because
OPFS refused the second host open. The re-scan after the await is what establishes "one record per
file" for the first time, and it needs no new scheduler primitive because nothing between the
re-scan and the bind suspends — whichever coroutine resumes first installs its record atomically
with respect to the other, and the loser always sees it. On OPFS the loser's own open is exactly
the one that failed, so the failure becomes a share.

Two consequences worth writing down. `/proc` snapshots at open, so two concurrent readers of one
`/proc` file now see one snapshot; a per-backend opt-out was rejected because it reintroduces the
backend-dependent rule §5.2 refused. And two concurrent `O_WRITE|O_TRUNC` opens on MemFs leave the
loser's truncation already done before the re-scan refuses it — true before this change as well,
where the loser also truncated *and* succeeded, so it is strictly less bad.

### The other half: `Input` opens lazily

The VFS change alone fixes `cat bar bar`, but `Input` was also opening every named file up front,
which is what made one command hold two descriptors on one path. It now opens each file when the
read reaches it and closes it before the next, so at most one named file is open at a time. The
header already claimed this ("they are opened lazily") while the implementation did the opposite.

The price is real: `cat good missing` now prints `good`'s bytes *before* reporting that `missing`
does not exist, where the up-front open reported it first. `head -n 2 good missing` never mentions
`missing` at all, because it stops before reaching it. Both are the honest consequence of laziness
rather than something to special-case.

`open_all` was deleted rather than renamed — it no longer opened anything, and the only state it
still set belongs in the constructor, which now takes `who` as a third argument. That is an
SDK-visible change to `include/braam/proc/io.h`. The diagnostic moved into `read()`, which is the
only place that knows the file's name; every caller already mapped a non-`Closed` error onto an
exit status without printing, so none of the nine needed its loop touched.

The two changes are complementary, and it is worth being precise about which covers what. Laziness
fixes one process naming one file twice. Sharing is what fixes `grep one notes < notes` — where
`Sys::Spawn` moves the redirection's handle into the child, which then opens the same path itself —
along with `cat bar | grep x bar` and two processes on one file. The end-to-end test carries both.

Regression coverage is the uncomfortable part. `test/fakefs.mjs` does not model OPFS's exclusive
lock and was deliberately left permissive, so a shared-handle path that regressed into issuing two
host opens would sail through the whole smoke test while a real browser threw
`NoModificationAllowedError`. `test_vfs`'s `CountingFs` counts what reaches the filesystem — two
descriptors must be one `open()` and one `close()` — and is the only thing standing between that
regression and a shipped build. The await window itself is not reachable from `tests.wasm`:
`run_now` panics on a suspension and every filesystem the suite mounts answers synchronously.

Supersedes "One open handle per file" below, and the eight files in "What a syscall costs,
measured" no longer need to be different.

## A canvas is focusable, not editable

On a tablet the prompt appeared, a finger could select text, and nothing could be typed: the
software keyboard never came up. The canvas *was* focusable — `tabindex="0"`, and `mount()` set it
defensively as well — and that is the whole of the bug. "Focusable" and "editable" are different
predicates and only the second raises a keyboard. Every key in the system entered through one
`keydown` listener on an element that could never receive one from a touch device. Selection
worked because it rides pointer events, which do fire for touch, which is what made the failure
look like a keyboard problem rather than a focus problem.

So the focus moved to a hidden `<textarea>` that `mount()` creates, and the canvas gives its focus
away (`canvas.addEventListener("focus", focusSink)`) rather than taking it.

**A textarea, not an `<input>`.** On both iPadOS and Android an input's return key is "Go" or
"Done" and blurs or submits instead of inserting a line break — so `insertLineBreak` never arrives
and Enter is unreachable from the soft keyboard. That decides it on its own. `contenteditable` was
the other candidate and loses on the housekeeping: the browser injects `<br>`/`<div>` wrappers to
keep clearing, and `inputmode`/`enterkeyhint`/`autocapitalize` are not uniformly honoured on it.

**Every declaration on the sink is load-bearing**, and each of them looks like something to tidy
up later:

- `opacity: 0` and not `display: none`, `visibility: hidden`, `hidden`, `readonly` or `disabled` —
  every one of those makes it unfocusable or kills the keyboard.
- `font-size: 16px` on an invisible element, because iOS Safari zooms the page when focus lands on
  a field under 16px. That is the correct fix; `maximum-scale=1` in the viewport meta is the other
  one and it disables pinch-zoom for everyone.
- Positioned at the viewport origin rather than off-screen. `top: -9999px` is the usual trick and
  is wrong here: iOS scrolls the focused field into view and would take the terminal with it. A 1×1
  transparent box already in view gives scroll-into-view nothing to do.
- `pointer-events: none`, so the 1px corner cannot eat a tap meant for the canvas.
- `position: fixed` on `document.body` rather than a wrapper around the canvas. Wrapping reparents
  an element the page laid out: in `embed.html` the canvas is the `1fr` row of a two-row grid sized
  by `height: 100%`, and a `<div>` between them leaves that `100%` resolving against nothing — the
  terminal collapses. Fixed positioning is out of flow, and `dispose()` is one `remove()`. It is
  the shape `filePicker()` already had.

### Two sources, one destination, and the rule between them

A soft keyboard frequently reports no key at all: GBoard sends `keyCode 229` with
`key: "Unidentified"` for ordinary letters. iPadOS is better and sends real `keydown`s for typing —
but dictation, predictive text and autocorrect arrive there only as `input` events. So a second
route was needed on both platforms, for different reasons, and the risk it brings is a keystroke
delivered twice.

The invariant that prevents it: **the text route runs exactly when the key route did not prevent
the default.** `preventDefault()` on a keydown suppresses the insertion and therefore the
`beforeinput` and `input` that would have followed, and `consumes()` returns true for every
printable key. Its two early-outs are the whole risk surface and both are covered — `key === null`
is the *intended* handoff (and now also makes a Mac dead key compose, which was silently dropped
before), and the `metaKey`/`RESERVED` cases cannot produce an insertion the handler does not
filter. The `keyCode === 229` guard at the top of `onKeyDown` is the converse: those keydowns post
nothing and prevent nothing, so the text route gets them.

The text route reads `sink.value` rather than `event.data`, because the firing order of `input` and
`compositionend` differs between engines — Chrome fires `input(insertCompositionText)` first,
Safari has fired it after. Reading and clearing the value makes the order irrelevant: whichever
handler arrives first takes the text and empties the field, and the other finds nothing. Deletion
is the exception and is taken on `beforeinput`, because a delete on an already-empty field changes
nothing, and a UA that changes nothing fires no `input`.

### Why the text route feeds a paste run

Reusing `{kind:"paste"}` means no new worker message kind and no worker change at all, and it
inherits the pacing against `key()`'s return value that a dictated sentence needs. The third
reason is the one that is easy to miss: `web/worker.js` dispatches a `{kind:"key"}` straight into
`kernel.key()`, *ahead* of a `pasting` run still being fed. That is deliberate and documented
there — `^C` must not wait behind a paste — but it means a backspace posted as a key while the
word before it was still draining would arrive first. So text, `Enter`, backspace and delete all
go as a run, and the single deliberate exception is the character after a latched `Ctrl`, which
uses `sendKey` *because* it jumps the queue: that one is `^C`.

Nothing here crosses the wasm boundary. A `text(ptr, len)` export would be the same inversion that
"Paste, as typing" already rejected for a `paste()` export — a byte stream into the keyboard, with
§2.3 turned inside out — and the exact-surface assertion in `test/run.mjs` went unedited, which is
the evidence the change stayed on the page.

### The key bar, and who owns its DOM

Fixing the keyboard still leaves a tablet unable to send `^C`: neither iPadOS nor GBoard offers
`Ctrl` or `Esc`, and most layouts have no `Tab` and no arrows. Nothing in the input pipeline can
synthesise a key the keyboard does not have, so the page supplies them.

**The page places the container and `mount()` fills it** (`mount({canvas, keys})`). The
alternative — `mount()` creating and inserting the bar — breaks `embed.html`'s two-row grid, and
forces this file to inject styling into a page whose theme it does not know; `braam.js` sets no CSS
on anything the embedder owns, and that would have been the first exception. With a page-placed
container the layout is the page's and the existing `ResizeObserver` does the rest for free: the
bar is a real flex item, so when it appears the canvas shrinks, the observer fires and the grid
re-lays out. `dispose()` gets an unambiguous rule too — remove the buttons `mount()` made, leave
the container it did not, which is `ownPicker` again.

Showing it is a CSS media query in the page, `any-pointer: coarse` rather than `pointer: coarse`:
an iPad with a Magic Keyboard reports a fine primary pointer and still has a touchscreen its owner
may type on. The worst case is a bar a keyboard user ignores. `navigator.maxTouchPoints` is the
same information but static, and reads true under desktop device emulation. Because the container
is `display: none` on desktop it has zero height, so the desktop terminal is unchanged.

`Ctrl` latches onto the next key and clears; a second tap toggles it off and a blur clears it.
**No lock-on-double-tap** — a stuck `Ctrl` is a terminal that appears broken, and there is nowhere
on a seven-button bar to explain a second state. Routing the latch through the text route as well
as through `onKeyDown` is load-bearing: on Android the `c` of `^C` arrives as input text, not as a
keystroke.

The one genuinely delicate part is that **tapping a button must not dismiss the keyboard**.
Preventing `mousedown`'s default is what holds the focus on the sink; it is the toolbar-button
idiom every rich-text editor uses, and a tap synthesises compatibility mouse events before its
click. `preventDefault()` on `pointerdown` is *specified* to suppress those while still firing
`click`, but an engine that got that wrong would leave the bar silently dead on exactly the
platform it exists for. Acting on `click` also gives mouse, touch and keyboard activation one path
with no double-fire, and `touch-action: manipulation` removes the latency that would otherwise
cost.

### The keyboard covers the page rather than resizing it

None of this went into `braam.js`. Resizing or scrolling an embedder's document is the layout
meddling the file refuses everywhere else, and it does not need to: the `ResizeObserver` already
reflows the grid the moment the canvas box changes, so a page that shrinks correctly gets a
correct terminal for free. What `index.html` does is `interactive-widget=resizes-content` for
Android — the default is `resizes-visual`, which slides the page instead — `100dvh` for the URL
bar, and a `visualViewport` listener for iOS, which shrinks neither the layout nor the dynamic
viewport for its keyboard and appears to ignore `interactive-widget` entirely. `embed.html` takes
the meta and the `dvh` and not the listener, since page-fitting logic muddies what that page is
there to show.

### Three things left uncertain on purpose

Each has a contingency that should not be paid for speculatively, and each is a question only a
real device answers:

- **Android backspace on an empty field.** Some GBoard versions send a real `keydown`/`keyCode 8`
  even while sending 229 for letters; others send only `beforeinput(deleteContentBackward)`. Both
  are handled. If some UA suppresses even the `beforeinput`, the fix is the sentinel CodeMirror
  uses — seed the sink with two no-break spaces, keep the caret between them, and diff on drain —
  which costs autocorrect context and makes a screen reader read the sentinel.
- **GBoard composing whole words.** `spellcheck="false"` and `autocorrect="off"` make it much less
  inclined to, but if it does, the echo lags to word boundaries. It works; it lags. The fix would
  be an incremental emitter diffing `sink.value` against what has already been sent.
- **Re-raising a keyboard the user dismissed.** The sink still holds the focus, so a tap focuses an
  already-focused element, which on iOS does not bring the keyboard back. A `blur()` before the
  `focus()` is the candidate and may not work either, at the cost of a flicker on every tap.

### Verification

There is no browser harness in this tree, so the three CTest cases prove only what they always
prove — and here that is the point: `smoke` asserting the exact import and export surface is the
check that this stayed on the page side. The behaviour was checked by hand, in a browser and on a
tablet, against the list in the section above.

## Concept.md says what the system is, not how it got here

The spec had accumulated its own history. Half of it was written in the past tense about the
present: "M5's `/bin` was `BinFs`", "this section once said it would", "a `host_fetch` was on the
list too, and that is not what M6 built", "the cost is paid twice over" with the byte counts of a
model that no longer exists. Every one of those was true and every one of them was an amendment
narrated rather than applied — a reader had to follow the argument to the end to find out what the
rule *is*. It is 1,303 lines down to about 950, and what went was the narration, not a decision.

The division of labour is the one CLAUDE.md already states and the spec was not keeping: this file
holds the *why*, Concept.md holds the *what*. So a paragraph that ended in a rule keeps the rule
and loses the case for it, because the case is here under the note that argued it. Nothing was
deleted that is not either still in the spec as a statement or already in this file as a section —
the applet's byte counts are in "One program model", `BinFs` is in M5 and M6, the unbuilt
`host_random` is in "System_Calls.md, and what writing it found", and the measured syscall cost is
in "What a syscall costs, measured".

**Nothing was renumbered**, because the numbering is cited from source comments — thirty-one
citations of §4.3 alone. Two sections changed what they contain instead:

- **§6 was "Milestones" and is now "Host services".** The milestones were a pointer to this file
  and a note that the section kept its number, which is a heading holding a place for nothing.
  `src/proc/io.h` already cited §6 for the wall clock — a citation that meant M6 and landed on the
  milestone list — so filling §6 with fetch, WebSocket, the clipboard, file transfer and the clock
  makes an existing citation right rather than breaking a live one. The material came from the
  scattered halves of §3.4 and §5.4 that described those services and never named them together.
- **Appendix D was provenance** — three earlier design notes consolidated, and the one
  disagreement between them resolved in favour of the process model. That is the definition of
  history in a spec, and it is deleted rather than moved: the notes are gone, the disagreement was
  settled two milestones before every program became a binary, and §4 states the outcome flatly.

**Reading it against the source found four numbers that had rotted**, which is the argument for
the trim in one line: a spec that narrates is a spec nobody re-checks. `PROC_TASKS` was quoted as
four and is eight; the child bounds were "eight live children, eight levels deep" and are
`SYS_CHILD_MAX` sixteen and `SYS_PROC_DEPTH` eight; the renderer was "under 200 lines" and is
about 300. All three now name the constant or round honestly, because a constant that moves
should not need this document edited. (CLAUDE.md repeats the `PROC_TASKS` figure and is wrong the
same way.) The fourth was §7's repository layout, which listed no `examples/` and no
`Programming_Manual.md`.

Three other things changed in kind rather than in length. The §3 diagram now draws the process
worker with its imports and exports, since a process is a peer of the kernel worker and was
described in prose under a "(M9+)" label. §4.3's rules about the operation table were four
paragraphs of when-and-why and are now four named rules — a caller in `src/cmd/`, the synchronous
half closed at four, a stream is a descriptor, and text under `/proc` is not a syscall — which is
what someone adding an operation actually needs to check. And §8 lost "early" from its title: the
five things in it are standing constraints, not advice to a project that has not started.

## The plan is deleted, and its figures are here

`doc/TODO.md` was the plan for putting every program in a worker of its own — nine items, T1 to T9,
written before anything moved and annotated as each was finished. Every one of them has a note of
its own further down this file, so what the plan still held once T9 was done was a to-do list with
nothing to do, plus two things that were not written anywhere else: **the measured figures**, and
**the one measurement that was never taken**. It is deleted and those are here. The T-numbers are
kept as citations — this file and CLAUDE.md refer to "the plan's T5" and mean this — and there is
nothing left in the plan that is a work item.

The figures are a record, not something to re-run: `make bench` was the harness, and it was deleted
with tier 2 ("Tier 2 is deleted" below), because its three arms were an A/B between two program
models and there is one. The counters it read are still there and still unconditional —
`makeProc`'s `stats()`, and `paint`/`tick` plus the `stats` and `render` messages in
`web/worker.js` — so a page that wants to measure again has what it needs.

### The method, and why it is a difference

"What a syscall costs, measured" below has the reasoning in full; the short of it is that the
number is ΔT/ΔN over two runs of the *same command* — `wc` over one file against `wc` over eight —
so the spawn, the compile-cache hit, the instantiate, the exit and the prompt redraw all subtract
out, and ΔN is read out of counters rather than predicted from `SYS_CHUNK`. Both runs below are ten
timed runs after two warm-ups, medians, on the same 8-core Mac, in three engines. Gecko and WebKit
step `performance.now()` in whole milliseconds without cross-origin isolation, so their columns are
quantised — 297 round trips into a 1 ms grid is a ±3.4 µs quantum, and a 0.00 ms keystroke is a
clock step rather than a free one.

### T1, before anything moved — 0.2.44-e6a8552

A round trip, in microseconds:

| | Blink (Vivaldi) | Gecko (Firefox) | WebKit (Safari) |
|---|---|---|---|
| every program at tier 2 (as it shipped then) | 40.2 | 80.8 | 5.1 |
| the same, less the every-64th timer wait | 6.2 | 16.8 | 1.7 |
| every program at tier 3 | 42.3 | 33.7 | 33.7 |

So §4.4's *"order 0.1 ms"* was two to three times pessimistic and a worker of one's own cost about
**30–40 µs a round trip**: `wc` of 86 KB went from 4.2 ms to 11.4 ms in Blink and 6.5 to 12 in
WebKit. The middle row is the finding that mattered most at the time — most of tier 2's bulk cost
was `web/worker.js`'s every-64th `setTimeout(drain, 0)`, which waited 1.3 ms in Blink, 2.4 in Gecko
and 0.25 in WebKit, and which a worker of one's own never took.

A keystroke, in milliseconds, key to the last repaint it caused:

| | Blink | Gecko | WebKit |
|---|---|---|---|
| tier-2 shell | 0.17 | 1.66 | 0.36 |
| tier-3 shell | 0.63 | 1.89 | 3.26 |
| 64 keys back to back, per key | 0.10 → 0.34 | 0.42 → 2.70 | 0.27 → 5.09 |

Nothing was dropped at either tier in any engine — the ring holds 64 — but sustained typing at
2.7 ms a key in Gecko and 5.1 in WebKit is a shell that feels slow while it is being typed into,
and that is the number T7 and T8 had to answer to. WebKit's pass was marked tainted (its tab lost
focus mid-session), so its absolute milliseconds are the weakest column here; its round-trip and
keystroke *ratios* agree with the other two, which is the check that matters.

### T5, with thirty-one programs moved — 0.2.47-8ca8053

The gate on the other side of T3, and the decision not to write T6. Same method, three arms —
every program at tier 2, tier 3 but for the shell (what shipped then), tier 3 including the shell
(what ships now) — with T1's figure in brackets. No pass was tainted and no keystroke was dropped.

A round trip, in microseconds:

| | Blink | Gecko | WebKit |
|---|---|---|---|
| every program at tier 2 | 41.8 (40.2) | 79.1 (80.8) | 10.1 (5.1) |
| tier 3 but for the shell | **44.9** (42.3) | **38.7** (33.7) | **33.7** (33.7) |
| tier 3, shell included | 47.0 (44.4) | 40.4 (38.7) | 35.4 (38.7) |

WebKit's 5.1 → 10.1 on the first row is one grid step, not a regression.

A keystroke, in milliseconds:

| | Blink | Gecko | WebKit |
|---|---|---|---|
| every program at tier 2 | 0.20 | 2.00 | 0.00 |
| tier 3 but for the shell | **0.20** | **2.00** | **0.00** |
| tier 3, shell included | 0.65 | 2.00 | 3.00 |
| 64 keys, per key — tier 3 but for the shell | 0.09–0.12 | 0.41–0.42 | 0.25–0.27 |
| 64 keys, per key — tier 3, shell included | 0.27–0.28 | 2.58–2.62 | 6.20–6.25 |

The shipped row was the tier-2 control's row to the clock in all three engines, which is what says
the keystroke's cost was the *shell's* tier and not the programs'. T1's alarming figures were never
the shipped system's; they were the tier-3-shell arm's, and in WebKit that arm got *worse* on the
re-run.

Bulk I/O, in milliseconds, which is what the T6 decision turned on:

| | Blink | Gecko | WebKit |
|---|---|---|---|
| `wc /bin/sh` — tier 2 | 4.2 (4.2) | 9.5 (9.0) | 6.0 (6.5) |
| `wc /bin/sh` — tier 3 but for the shell | 11.0 (11.4) | 15.5 (16.0) | 12.0 (12.0) |
| `wc` over eight files — tier 2 | 16.6 (16.2) | 33.0 (33.0) | 9.0 (8.0) |
| `wc` over eight files — tier 3 but for the shell | 24.4 (24.0) | **27.0** (26.0) | 22.0 (22.0) |
| `cat /bin/sh \| cat \| wc` — tier 2 | 7.8 (7.7) | 13.0 (13.0) | 9.5 (11.5) |
| `cat /bin/sh \| cat \| wc` — tier 3 but for the shell | 19.7 (25.6) | 26.0 (30.0) | 19.0 (27.0) |

The tier cost **10–13 ms** on the heaviest thing in the suite — a quarter of a megabyte through
three processes — and 6–7 ms on an 86 KB file. In Gecko `wc` over eight files was *faster* than
with everything at tier 2, because the every-64th timer had retired itself: as shipped that command
took the slow route zero times, since the only tier-2 process left was the shell and it takes 21
steps rather than 483. All of it is round trips and none of it is the worker — `true`, one spawn
and one exit with no I/O, cost the same either way to within 1.5 ms in every engine, because the
pool has a worker waiting.

### The bar T8 was taken against

Sustained typing, 64 keys back to back, milliseconds per key:

| | tier-2 shell (T5) | tier-3 shell as T5 measured | bar |
|---|---|---|---|
| Blink | 0.09–0.12 | 0.27–0.28 | ≤ 0.20 |
| Gecko | 0.41–0.42 | 2.58–2.62 | ≤ 0.90 |
| WebKit | 0.25–0.27 | 6.20–6.25 | ≤ 0.90 |

plus zero dropped keystrokes and no regression beyond the clock on the bulk figures. "Under a
frame" was explicitly *not* the bar: 6.2 ms a key is already under a frame, and T5 called it a
shell that feels slow, correctly. Cutting the keystroke from five round trips to two ("A repaint is
one syscall") is what got under it.

### The measurement that was never taken

T8 instrumented an attribution A/B and never ran it, and the harness for it is gone. The question
is live and worth stating, because the arithmetic does not close: **five round trips at the
measured 34–45 µs is 0.25 ms, which was Blink's tier-3 keystroke to the clock, 13× short of
Gecko's and 35× short of WebKit's.** So Blink's keystroke *was* its round trips and the other two
engines' was not. The only per-turn asymmetry between the prompt's round trips and `wc`'s anywhere
in the tree is that the prompt's damage the grid and the kernel worker presents once per tick — a
repaint of four operations painted the same keystroke three times, which is why the cursor had to
be hidden through one or it would be seen walking the line. The A/B was a burst with drawing turned
off against the same burst lit: if the dark one drops to the lit one's figure the residual is the
canvas commit per task, and if it does not, the bare worker↔worker turn is slow on that path.

`echo` has since made the repaint one operation and one present, so whichever answer it would have
given, most of what it was measuring is no longer being paid.

### T6, which was never started and should not be started on a guess

T5 decided against it and the decision stands: what would reopen it is a workload, not a figure —
something moving megabytes rather than a quarter of one, where 34–45 µs per 512 bytes is 70–90 ms
a megabyte. Neither of its two shapes is cheap, and this is what they were:

- **A bigger `SYS_CHUNK`.** It is 512 because that is the allocator's top size class on both sides
  of the wire (§8.2); raising it costs a 64 KiB span per buffer, so it is an allocator change as
  much as an ABI one.
- **Batching the replies.** The step protocol already carries a *list* of parked calls up and
  exactly one reply down — `_resume(token, ptr, len)` answers one call. Batching means the kernel
  coalescing the steps it owes one pid, both halves of `web/proc.js`, `web/procworker.js`,
  `test/fakeworker.mjs`, and an amendment to System_Calls.md.

---

## A prompt is one syscall

The plan's T8 cut the keystroke and said outright what it had not cut: `anchor()` is seven round
trips, `interactive()` adds a `cwd_get`, and *"Enter to the next prompt is an order of magnitude
more than a keystroke — the next thing anyone will notice"*. This is that.

**Enter to the next prompt was twelve round trips and is five**, measured the way the keystroke
was — `net.proc.stats().calls` across one Enter at an empty prompt in `test/run.mjs`'s driver
reads 12 before and 5 after, and a keystroke reads 2 either side. Both spans run from one parked
`key_read` to the next, so the five are the `Echo` that repaints the committed line, the newline
that ends its row, the `cwd_get` the next prompt names, the `Echo` that draws it, and the
`key_read`. The seven that went were `anchor()`'s: a `cursor` get, a `style`+`write` for the
directory, a `style`+`write` for the prompt proper, a `style` to put the default back, a second
`cursor` get — nine when a failed status put a fourth colour in front — plus the `cursor` set that
followed it.

**`Sys::Echo`'s bytes are a sequence of styled runs.** A run is a style word and a length, every
header ahead of every byte; `SYS_ECHO_FRESH` and `SYS_ECHO_END` in the op word carry what the two
`cursor` gets were for. The §4.3 table is still thirty-six and `PROC_ABI` is 8. `anchor()` is one
call with four runs — red status, white-on-blue directory, bright-white prompt, and a run with no
bytes that puts the default back — and `redraw()` is one run of `SYS_STYLE_KEEP`, which is why the
keystroke is unchanged rather than merely un-regressed.

Four things are worth recording about the shape it took.

- **`SYS_STYLE_KEEP` is load-bearing, not a convenience.** Without a style that means *leave the
  sticky one alone*, a run could not be exactly a `Write`, and `Echo` would stop being a
  composition of operations that already exist — which is the sentence the previous note rests on
  and the constraint this one was designed under. A run is a `Style` and a `Write`, `FRESH` is a
  `Cursor` get and a conditional newline, `END` is not calling `Cursor` afterwards. Nothing here
  reaches a cell, a claim or a stream the caller could not already reach.
- **It fixed a colour bug nobody had reported.** `blank()` takes the *current* `g_fg`/`g_bg`, and
  `scroll()` clears the new bottom row with it. The old `anchor()` put its leading newline inside
  the first non-empty coloured run, so with a directory and no failed status that newline went out
  white-on-blue — and every prompt drawn at the bottom of a full screen left a blue bar from the
  `$` to the right margin. `FRESH` writes it before any run's style. It is the concrete form of
  "whoever sets a colour puts the default back", violated for exactly one byte, and `test/run.mjs`
  pins it now: six `echo -n`s on a 24×4 grid, then every column past the prompt must be on black.
- **Two bits rather than one.** `FRESH` and `END` answer different questions — where the write
  starts, where the cursor rests — and each independently kills one payload field, `FRESH` the
  anchor and `END` the cursor offset. One combined bit would have retired all three fields at once
  and left the next reader wondering why they were in the payload at all. Neither is a sentinel in
  `x`/`y` either: `FRESH` is an action, not a coordinate, and a sentinel would be invisible in the
  constants table.
- **The `cwd_get` stays, and that is a decision.** A prompt at four round trips instead of five is
  available for the price of `cd` stashing what `cwd_set` already hands back. It was refused when
  the directory went into the prompt and it is refused again for the same reason: `cd` being the
  only thing that moves the shell is true today and is not a property anything enforces, and a
  wrong prompt is believed. Five is the floor while that holds.

The rejected alternatives. **Keeping a thin `cursor_echo(…, bool on, Str s)` overload** for
`redraw()` would have made `cursor_echo(x, y, cur, 1, s)` convert to both `bool` and `u32` — a
trap `-Werror` does not catch — and it could not have been a plain forwarder, because a `Task` is
eager and a local `StyledRun` on a forwarder's stack would be correct only by accident; making it
a real coroutine costs a second frame per keystroke, on the one path this change exists to
protect. `redraw()` builds a one-element array instead. **Deleting `Cursor` and `Style`**, which
this leaves with no caller in the tree, was refused: `Echo` is their fused form for the caller
that pays a round trip apiece, not their replacement, and a program colouring a word on stdout has
no anchor to name and does not want a row of its own. Deleting now is free and re-adding later is
an ABI bump that invalidates every stamped binary and every installed SDK. **A third bit `HERE`**
— anchor at the cursor, no newline — would have let `Echo` subsume `Style`+`Write` and taken the
table to thirty-four, but it makes the cheap thing require the expensive shape and adds a bit with
no caller in `src/cmd/`, breaking the rule it is invoked to satisfy. **Interleaving the run
headers with their bytes** costs the single bounded validation pass: with the headers first the
kernel checks the count, the table and the summed lengths before it reads a byte, rather than
deriving each bound from the previous read the way `argv_at` has to.

One correction while here: the alignment argument for headers-first, which is the obvious one, is
false in this codebase. `sys_get_u32` is four byte loads and nothing in a staged payload is ever
an aligned access — `ScreenBlit` is the one place alignment matters, and that is why *its* header
is a fixed seven words.

---

## Tier 2 is deleted

Every binary in the tree has asked for a worker of its own since T8, and `stamp.py --tier` has
been required rather than defaulted since the same change — so what was left of the second
program model was a *fallback*: the path a host took when it could not make a worker. It cost a
second syscall path in `web/proc.js`, a deferral mechanism in `web/worker.js` that existed only
to drain it, a three-armed bench harness, a word on the wire, and a public knob in the SDK. All
of it stood behind a case nothing in the tree exercised and no test could reach without turning
the workers off by hand.

It is gone. One program model, one syscall path, and §4's table has one row.

### The word leaves the wire

`ProcMeta` is five `u32`s rather than six and `PROC_ABI` is 7. The alternative was keeping the
field and requiring 3, which is a smaller diff and leaves a one-value enum on the ABI for a
reader to wonder about. The field's second job — `Tier::Retired = 1`, so a binary from before M8
was refused rather than misread — is what the `abi` word does, and does for every amendment
rather than for that one, so nothing is reserved in its place.

`proc_pack` gets simpler with it: the tier was a nibble at the top of the flags word, which is
why `max_pages` had twelve bits and a `static_assert` defending them. It has sixteen now.

### A wait, not a fallback

The question the fallback answered still has to be answered: what does a host that cannot make a
nested worker do? The choice taken is to **pause and retry** — 10 ms, then 20, 50, 100, 200, 500,
and a second from there on, printing `no worker, retrying` on the program's own stderr each
time — rather than fail the command or refuse at boot.

Failing the command was the obvious alternative and is worse in the case that matters: the
process most likely to want a worker when none can be had is `/bin/sh` itself, at boot or after
`dropWorkers()`, and a session that ends with one line is less useful than one that says what it
is waiting for and starts the moment it can. Refusing at boot with a "this browser cannot run
Braam" screen was the other, and it decides too early: a host that has *lost* its workers is not
a host that never had them, and the retry covers both without having to tell them apart.

The loop lives in `spawn_process` in `src/user/exec.cpp` — a coroutine of its own rather than
straight-line code inside `exec_process`, because `exec_process`'s frame is on the path
`test_shell` guards and a frame past 512 bytes costs a whole 64 KiB span. `sleep_ms` is
cancellation-aware like every awaitable since M1, so `^C` abandons a wait and a cancelled job
unwinds it with nothing written for it.

One wrinkle decided the shape: `proc_spawn` takes `String &&image` and the request record adopts
it, which is §4.4's "a process image is tens of kilobytes and the caller has one already". A
retry therefore has no image left. Re-reading the file with `read_file` before each attempt after
the first was preferred to passing a `Str` and copying, because that copy would be paid on every
`exec` in exchange for a path that only runs when the system is already degraded and has ten
milliseconds to spare.

Init's respawn bound is untouched and is no longer reachable this way: a shell that is *waiting*
for a worker has not started, so it is not one of the three deaths that end a session.

### The latch had to go with it

`web/proc.js` kept a `workers` boolean, set false the first time a worker refused to be made or
failed to load, after which nothing hired again. That was right when the answer to no worker was
a permanent fallback; it is wrong when the answer is to ask again, since a latch that never
clears makes recovery impossible. `hire()` now tries each time and the kernel's backoff is what
keeps the asking cheap.

The cost is one dead worker per hire on a host whose `procworker.js` will not load, and
`test/run.mjs` asserts exactly that rather than leaving it implied. Note the asymmetry it
exposes, which is not new but is now the only behaviour: a worker that is never *made* is
`Again` and retried, while one that is made and then fails to load is a process that **died**,
because by then the spawn has been answered and the program is in its first step. The second is
bounded by init's three respawns, not by the backoff.

`link.ready` went with the latch — it had no other reader. The `{ k: "ready" }` message stays on
the wire, dropped on the floor as it was before T2 gave it a job.

### What the bench measured is not measurable any more

`make bench` existed to A/B the two tiers: `bundle2.bin` and `bundle3nosh.bin`, packed by
re-stamping the staged binaries, against the shipped archive. With one tier there is no A/B, so
the cmake target, `web/bench.html`, `web/bench.js` and `tools/bench.mjs` are deleted, along with
the `defer` counters in `web/worker.js` that only its arms read. T1, T5 and T8's figures stay
recorded at the top of this file, under the plan they belonged to.

Deleting `tools/release.py`'s `SKIP` set resolved a live bug on the way: it named `bundle3.bin`
while the cmake target emitted `bundle3nosh.bin`, so that twin was packed into the released site
zip whenever `bench` had been run in the same tree.

### What it costs elsewhere

M8's acceptance criterion "tier selection comes from binary metadata" is retired rather than
broken: there is no selection. The criteria list below keeps it as the record of what M8 built.

The kernel grew by 1,297 bytes — 142,473 to 143,770, against a 256 KiB budget — which is the
retry coroutine costing more than the tier checks and the `Tier` enum saved. `bundle.bin` lost
128 bytes, four from each binary's section, and rounds to the same 497 KB because the archive is
block-aligned.

`test/run.mjs` gained a case and lost one. The old fallback case — workers off, `dropWorkers()`,
then assert a program still runs — asserted the thing being deleted; in its place the same setup
asserts the retry: the shell dies, the line appears, the backoff prints two more on the driver's
own clock, nothing binds a worker while there is none to bind, and the session comes back when
`net.workers` goes true again. It is still last but for `exit`, for the reason it always was.

---

## The shell takes a worker

The plan's T8, and the end of "every program at tier 3": `set(BRAAM_BIN_TIER_sh 2)` is gone,
`src/cmd/CMakeLists.txt` passes no `TIER` at all, and there is no name left in §4's tier table.
`stamp.py`'s `--tier` is required rather than defaulting to 2, which had become the answer no
caller wants.

**Two things had to be true first, and neither was about the stamp.** T7 made a lost worker cost a
shell rather than the session — init replaces one that *died* — and the section above made a
keystroke two round trips where it was five. The order was not incidental: T1 and T5 both measured
a tier-3 shell at 2.6 ms a key in Gecko and 6.2 in WebKit while it was being typed into, and §4.4
said in as many words that the prompt was the one program that could not afford the tier. Flipping
the stamp on its own would have shipped that.

**What the flip itself cost is nothing in C++.** The tier is a `u32` in a fixed-width custom
section, so no binary moved a byte and `bundle.bin` is the size it was.

### The bench arms turned over

`bundle.bin` is now the `t3` arm — every program at tier 3 — so `bundle3.bin` would have been a
duplicate of it, which is the mirror image of what T5 fixed for `bundle3nosh.bin`. The twin that is
now missing is the one that does *not* ship, so `make bench` packs `bundle3nosh.bin` instead, and it
takes a single re-stamp of `sh` rather than a pass over all thirty-two. The ids still mean what they
meant at T1, which is the point of them: `t2`, `t3nosh`, `t3`. Which of them ships has moved twice
and is not part of what they name.

### Four test cases changed meaning, not numbers

`dropWorkers()` takes the shell's worker now, and that is a different sentence in every case that
called it.

- **The held-step case is stronger than it was.** It asserts that a worker taken away with a step in
  it has that step *failed* by whoever took it. Before T8 a missed failure was a prompt that never
  came back; it is a session that never comes back now, because the shell parked for ever on a reply
  is the shell nobody will replace. The expected screen is `braam: the shell died` and a **bare**
  prompt rather than the killed command's status — the shell that would have printed the status died
  in the same breath — followed by a command on the replacement, which proves the tier came back
  too: `dropWorkers` lets go of the workers, not of the tier.
- **The broken-worker case had stopped testing what it said.** As written it set `net.broken` and
  then dropped the workers, which killed the shell into a world where every worker fails to load:
  init would re-exec, the replacement would die at its first step, `broke()` would set `workers`
  false, and the third shell would come up at tier 2 — so by the time the case ran its command the
  tier was already off and the command *succeeded* where the assertion wanted a crash. It empties
  the pool with a two-stage pipeline instead: one stage takes the last idle worker, the second has
  to hire and gets the broken link, and the shell keeps the good worker it was already holding. The
  case is about a program again.
- **`net.proc.kill(2)` reaches a worker** rather than merely dropping a record, so it asserts
  exactly one termination. That case is now literally what T8 risks instead of a stand-in for it.
- **The fallback case is where the shell dies for the last time**, and it needed one keystroke it
  did not need before: the kernel learns its shell is gone when it next tries to *step* it, and at a
  prompt the shell is parked on `key_read` with nothing outstanding to fail. So a key is spent
  provoking the death, and goes with the shell it reached.

**And the driver had to learn about `RESPAWN_TRIES`.** Init gives up after three deaths inside a
second of *scheduler* time, and scheduler time in `test/run.mjs` is whatever literal `run()` is
passed. The tail of the file killed the shell three times inside twelve milliseconds of it, which
ends the session at "the shell will not stay up" before `exit 7` is ever reached. The blocks are now
seconds apart on that clock, with a comment saying why, because the next case inserted there will
need the same spacing and nothing else would say so.

One thing that is not a test change but reads like one: `instantiate()` calls `net.proc.shutdown()`
now. `makeProc` is built once and outlives the three kernels the driver boots, and a reload throws
every nested worker away — so without it the outgoing shell's record was overwritten by the incoming
one at the same pid and its worker was orphaned, never pooled and never terminated. It moved the
pool literals (18 hired and 16 terminated at the first, 23 and 20 at the second) as much as the flip
did.

### What did not change

The wasm ABI, the §4.3 table, `web/proc.js`, `web/procworker.js`, and the process runtime. The two
tier-3 fidelity losses (§4.3) stop being true of thirty-one programs and start being true of
thirty-two: a binary that will not instantiate reads as a crash rather than a refusal, and
`Sys::Now` is relative. Nothing calls `proc_now()`, so the second is still a constraint on what may
be written next rather than a regression.

## A repaint is one syscall

The plan's T8, first half. T1 and T5 both measured the same thing and said it twice: a shell at
tier 3 costs 0.27 ms a key in Blink, 2.6 in Gecko and 6.2 in WebKit while it is being typed into,
against 0.09, 0.41 and 0.25 at tier 2. T5 called that "the strongest thing in this table" and §4.4
went further — *"the only program that cannot afford it is the one being typed into"*. So the flip
could not be a stamp change alone; the keystroke had to get cheaper first.

**A keystroke was five round trips and is two**, measured rather than counted: the fake driver's
`calls2` across one key at the prompt reads 5 before and 2 after. The five were `key_read`, then
`redraw()`'s `cursor` to the anchor, `write` of the whole line, `cursor` again to find out what had
scrolled, and `cursor` to put the cursor where the caller wanted it. Four operations, and **one**
change to the grid.

`Sys::Echo` at 71 is those four. Its payload is the anchor and how many cells past it to leave the
cursor, then the bytes; its reply is where the cursor ended, the geometry, and `scrolled`. `PROC_ABI`
is 6 and the §4.3 table is thirty-six.

Three things are worth recording about the shape it took.

- **`scrolled` is a counter, not an inference.** The old code wrote, asked where the write had ended,
  and subtracted that from where it should have ended — the shortfall being how far the grid had
  moved, since nothing counted scrolls. `screen_scrolled()` counts them now, and `Echo` reports the
  difference across itself. A resize's drop from the top is folded in at the same point, because
  that is also the grid moving up under an anchor, and the old inference could not see it at all
  once `cols` had changed underneath.
- **The dark-while-painting dance is gone.** `redraw()` hid the cursor before the write and showed
  it after, and the comment said why: each call was a step of its own, the grid is presented at the
  end of every tick, so a visible cursor would be *seen* walking the line. One operation is one
  tick. There is nothing to hide from, and a keystroke now paints the screen once where it painted
  it three times — which matters more than the round trips if what a tier-3 keystroke actually
  costs turns out to be the canvas commit rather than the transit.
- **It authorises nothing new.** Everything `Echo` does, four existing operations could already do,
  to the same screen, under the same refusal: `Err(Perm)` while another process holds the alternate
  screen, `Cursor`'s rule for `Cursor`'s reason. The bytes go through `p.io.out`, the same `Stream`
  `Sys::Write` uses, so a redirected stdout behaves exactly as it did. What it removes is three
  windows in which another process could move the cursor under a repaint in progress.

The rejected alternatives, since both are cheaper and neither is enough. **Skipping the post-write
`cursor` when the paint cannot reach the bottom row** is a real saving of one call in five and is
subsumed by this. **An append-only fast path** — write the one new character, no cursor calls at
all — is 5 → 2 for plain typing, but only for typing: backspace, the arrows, `^W`, `^U`, `^K` and
history recall all stay at five, and it costs the invariant that `redraw()` is one unconditional
repaint, which is what keeps the editor right across a resize, a `^L` and the deferred wrap column.

**What is still five is a whole line.** `anchor()` costs seven round trips — two `cursor_get`s and
three `put_styled`s of a `style` and a `write` each, nine when a failed status adds a fourth
colour — and `interactive()` adds a `cwd_get` per line. That is the Enter-to-next-prompt cost, and
it is the next thing anyone will notice; it is not this change, because it is paid once a line
rather than once a key. *(It was the next thing: "A prompt is one syscall", above, took the whole
line to five.)*

## The shell gets a pid, and `Sys::Fg` gets the rule it meant

Init ran `/bin/sh` from a default-constructed `Executable` and nothing ever filled `exe.pid` in, so
the shell answered to **0** — which is this system's word for *no pid*. `sched_spawn` returns 0
when it fails, `tty_keys_owner()` and `tty_screen_owner()` return 0 for "nobody holds it",
`SYS_WAIT_ANY` is 0, `Fg(0)` means "clear the foreground", and `link.pid = 0` is `web/proc.js`'s
"no process in this worker". One process answering to all of that is a collision waiting for T8,
which puts a worker behind that pid for the first time.

The shell now takes **init's** pid, because that is what it is: a process running inside init's
task rather than a job of its own. `main.cpp` passes a namespace-scope `u32` to `init_task` by
reference and fills it in once `sched_spawn` has said what it is — the same trick `exec.cpp` uses
to hand a stage its pid, and it works for the same reason: a `Task` is lazy, so the body has not
looked yet. Every replacement shell takes that pid again, which is safe because the record under it
is gone first: `~End` calls `proc_remove` and then `proc_kill`, and `proc_kill` is a bare `host_svc`
with nothing to await, so the host's entry is deleted inside that call rather than a tick later.

`/proc/<init>` grows a `cwd` line as a result, which is right — that job *is* the shell for all but
its first few ticks — and it is what `test/run.mjs` now asserts before killing the shell, so the
literal 2 in that case is justified rather than assumed.

### What the pid was hiding

`Sys::Fg` refuses a caller that does not own the terminal: *holding the raw keys, or being in front
itself, or nobody being in front*. A shell satisfies none of those from its second pipeline stage
onwards — it lets go of the keyboard before it spawns (a full-screen child claims the keys in its
first step, so handing them over afterwards is a race the child loses), and by then stage one is in
front. It passed anyway, because `tty_keys_owner()` returned 0 for "nobody" and the shell's pid was
also 0. **The check was being satisfied by a coincidence of sentinels**, and giving the shell a real
pid turned `^C` on a two-stage pipeline into a prompt with a stage still reading.

The repair is the clause the rule was missing: *or what is in front is what you put there*. The
console records who armed the foreground — one `u32`, set when the set goes from empty and cleared
with it — and `console_fg_owner()` is the fourth clause. That is a rule that says what it means,
rather than one inferred from a keyboard the caller has deliberately let go of. Concept.md §4.3 and
System_Calls.md both say so now.

Nothing had covered it: every `^C` case in the suite was a single-stage command, and arming stage
one is allowed whatever the guard says. `cat | wc` with a `^C` is the new case, and it fails
without the clause.

`console_fg_set` — the array form — went with the change. It had no caller but its own unit test,
left over from when the shell was kernel code, and a second path into the same state is a second
path to keep consistent.

### The replaced shell, covered rather than eyeballed

T7's respawn case proved a replacement appears. It now proves the replacement is a *whole* shell:
its job table is empty and what the dead one backgrounded went with it, `^C` at its prompt abandons
the line, `^C` on a foreground it armed itself cancels it, and `less` takes and gives back the
screen. It also moved ahead of the two tier-loss cases, so all of that runs at the tier the system
ships rather than at the tier-2 fallback.

Two things about that case are worth knowing before editing it. The shell has to be killed while a
**pipeline** is in front, not a single command: a lone foreground child clears the console on its
way out (`~Report`), while a pipeline's stages do not — nothing removes one pid from the set, so
the shell clearing it after collecting is the only thing that ever does, and that is precisely the
line the dead shell never reaches. And the `^C` has to be the **first** thing asked of the
replacement, because any command it runs would clear the stale set on its own way out. Both were
found by disabling init's `console_fg_clear()` and watching the case still pass.

---

## Init replaces a shell that died

The plan's T7, and the design question T8 is waiting on: what happens to `/bin/sh`
when the workers go. **Init starts another shell.** Concept.md §4 says so now, and this is why.

The question exists because §4's fallback is decided *before* a process starts. `exec` asks the
host for tier 3, the host says it has no worker, and the binary runs at tier 2 — one isolation
weaker, and userland does not notice. That covers a browser without nested workers and a
`procworker.js` that will not load at boot. It does not cover the tier going away *underneath* a
process: the instance is inside the worker, the worker is terminated, and there is no state to
carry back to tier 2. Every tier-3 process dies there. Today that costs a command. Once `/bin/sh`
is a tier-3 binary it costs the session, because init ran the shell once and printed *"reload to
start again"* when it ended.

### Why the honest answer and not the cheap one

T7 listed three. **Exempting `sh` from `dropWorkers`** is four lines, and it puts the string
`/bin/sh` into `web/proc.js` — a layer that has never known a program's name, and whose whole
discipline is that a pid is bound into a closure rather than a name being checked. It also answers
the wrong half of the question: the case that actually happens is the shell's *own* worker
breaking, which an exemption does nothing about. **Letting the session end** is defensible and free,
but it makes §4's fallback promise conditional on which program is asking, and the promise is the
reason the fallback is worth having.

So init notices. It is the only place that can: a shell is a process like any other, and the thing
that knows its child ended is its parent.

### Died, not exited — and why a status could not say which

`exec_process` returned an `i32` and nothing else, and an `i32` cannot carry the distinction. 132
is the kernel's word for a trap and `exit 132` is a program's word for whatever it likes; a shell
that a user typed `exit 130` at and one that was cancelled are the same number. So the function
gained a last defaulted out-parameter, `bool *died`, set true on entry and false at the one return
that reports a process ending on its own terms. Set that way round deliberately: a path added later
reports a death by default, rather than being silently forgotten in an enumeration.

The rule init applies is then a sentence rather than a table. **A shell that exited is final; a
shell that died is replaced.** `exit` at the prompt ends the session exactly as it did — there is
no login and no getty, and a prompt reappearing after `exit` reads as the command having failed.
A shell whose status is 130 is not replaced either: that is the kernel being disposed of, and a
cancelled init would otherwise spend its whole allowance restarting shells into a kernel that is
going away.

The replacement is an ordinary `exec` of `/bin/sh`, which is the load-bearing part. It asks for
whatever tier the binary claims and gets whatever the host can still give — after a worker that
would not load, `workers` is already false and the new shell is a tier-2 one. Nothing negotiates,
nothing is exempt, and §4's promise comes out true for the shell for the same reason it is true
for `wc`.

### Bounded, and why the bound resets

A shell that crashes on its own first step would otherwise announce it for ever. Three deaths in
quick succession and init says the shell will not stay up and stops. The bound counts *consecutive
fast* deaths, though: a shell that lived longer than a second starts the count again, because a
session that has been up all day should not be one crash away from being unrestartable. Both
numbers are constants at the top of `boot.cpp` and neither is precious.

Two smaller things go with the loop. The `Executable` is resolved on every pass — its image was
moved into the instance, so it cannot be reused, and a `/bin` repaired since the last shell died is
picked up for nothing. And `console_fg_clear()` runs between shells: a shell that died with a
pipeline armed leaves the console pointing at pids that are gone, and `^C` at the next prompt would
cancel that set instead of abandoning the line being typed. The keyboard and screen claims need no
such help — `~Proc` drops both, and a claim clears its route only if it is still the holder, which
is exactly the rule that makes a dead claimant safe.

`say()` now starts on a fresh row when the cursor is not at the margin. At boot it always was; a
shell that died left its prompt on that row, and what init has to say about it is not a
continuation of that prompt.

### The bug this found, which would have been the default answer

`dropWorkers()` terminated the worker holding a process and deleted the entry, and never touched
`p.pending` — the step the kernel was waiting on. `kill()` has always done both, and CLAUDE.md
states the rule: a terminated worker's in-flight step must be failed by whoever killed it. So the
*actual* status quo for a tier-3 shell was not "the session ends" but "the session freezes" — no
message, no prompt, the kernel parked for ever on a reply from a worker that no longer exists.
That is the worst of T7's three answers, and nothing had chosen it.

The fix is one shared helper called by both, so the two ways a worker is taken away cannot drift
again. `dropWorkers` keeps its meaning otherwise: it lets go of what the host holds and does not
set `workers` false, so a host that can still make workers gets tier 3 back on the next `exec`.
Init's replacement shell converges either way.

`test/run.mjs` has a case for it that fails loudly without the fix — a held step, then
`dropWorkers()`, then the prompt: unfixed, the assertion reports a blank row where `[1] home $`
should be, which is the freeze itself. The respawn has a case beside it: kill the shell's instance
from the host, press a key so it steps and learns, and assert the `braam: the shell died` line, a
fresh prompt, and a command running on the new shell. It kills pid 0 — init runs the shell with a
default-constructed `Executable`, so that is its pid — and asserts `live()` before and after, so
the case fails rather than quietly testing nothing if that ever stops being true.

Both cases run at tier 2, since T7 does not move the shell. They will mean more after T8 and are
written to keep working.

---

## The re-measurement, and why T6 is not being written

The plan's T5. T3 put thirty-one programs in workers of their own on the strength of
T1's figures; T5 is the gate on the other side of that, and it decides one thing — whether to spend
a week on T6, which is either a bigger `SYS_CHUNK` or a batched step protocol. **No.** The prompt
costs 0.09–0.42 ms a key under sustained typing and bulk I/O costs 10–13 ms more than tier 2 on the
largest workload in the suite, which are T5's own two conditions for skipping it. The figures are
at the top of this file; this is why they mean that.

### The harness had lost its control, which is the whole of the code change

T1's three arms were `bundle.bin` (every program at tier 2, as it shipped then) and two re-stamped
twins. T3 inverted the default, so `bundle.bin` became the tier-3-but-for-`sh` archive — which is
what `bundle3nosh.bin` already was. The two files were byte-identical, verifiable with `shasum`,
and the harness was measuring one configuration twice and the genuine tier-2 case not at all.

So the `bench` target packs `bundle2.bin` — every program stamped `--tier 2`, `sh` included — and
no longer packs `bundle3nosh.bin`, whose job `bundle.bin` now does. The staging directories are
`tier2` and `tier3` rather than `all` and `nosh`, which named a distinction that no longer exists.

The three arm *ids* were left alone deliberately: `t2` means every program at tier 2 exactly as it
did at T1, `t3nosh` means tier 3 but for the shell, `t3` means all of it. Only the file behind two
of them changed. That is what lets T5's tables be read straight against T1's, and it is worth more
than a tidier name — a benchmark whose arms silently change meaning between runs is worse than no
benchmark, which is precisely the failure this change repairs.

### What the numbers say that T1's could not

T1 could not separate the tier's cost from the shell's, because its tier-2 arm was also its
tier-2-shell arm. With a real control the two come apart:

- **The prompt is the tier-2 control's prompt, to the clock, in all three engines.** As shipped a
  keystroke echoes in 0.20 ms under Blink and under the 1 ms clock step in Gecko and WebKit, and 64
  keys back to back cost 0.09–0.42 ms each with nothing dropped. T1's frightening figures — 2.7 ms
  a key in Gecko, 5.1 in WebKit — were never the shipped system's. They belong to the arm with the
  shell at tier 3, where the re-run made WebKit worse still at 6.2 ms. That is T7 and T8's problem,
  and T5 sharpens rather than softens it.
- **A round trip is unchanged at 34–45 µs.** Nothing about putting thirty-one programs in workers
  made the per-call cost move, which is the result a cost model should give and is worth recording
  as a null.
- **T2's pool sizing bought 4–8 ms on a pipeline, in every engine.** `cat | cat | wc` fell from
  25.6 to 19.7 ms in Blink, 30 to 26 in Gecko, 27 to 19 in WebKit, and its counters went from
  `hired 1, reused 2, terminated 1` to `hired 0, reused 3, terminated 0` everywhere. The
  per-round-trip cost did not move, so all of that is the worker start T2 stopped paying — a
  cheaper win than T6 proposed, taken before T6 was decided.
- **T1's every-64th `setTimeout(drain, 0)` finding retired itself.** It was the larger half of a
  bulk command's cost at tier 2 — 9.5 ms of 16.6 in Blink, 18.5 of 33 in Gecko. As shipped, `wc`
  over eight files takes that route **zero** times, because the only tier-2 process left is the
  shell and it takes 21 steps rather than 483. T2 deferred the item on the grounds that it wanted
  its own measurement; this is that measurement, and it says the item shrank to the shell's own
  stepping. It is also why Gecko's shipped figure beats its tier-2 control outright.

### Why "acceptable" rather than a threshold

T5's criterion is a judgement and should be written down as one. 10–13 ms on a quarter of a
megabyte through three processes is under a frame for a command nobody waits on interactively, and
nothing in `src/cmd/` moves more data than that. The honest statement of the decision is therefore
not "the cost is small" but "nothing we have written can perceive it", and the thing that would
reopen T6 is a workload rather than a figure — something moving megabytes, where 34–45 µs per 512
bytes is 70–90 ms each.

Recording that distinction is the point of skipping T6 in writing rather than by silence.

---

## Every program in a worker of its own

The plan's T3 and T4. Thirty-one of the thirty-two binaries in `/bin` now ask for
tier 3, and `/bin/sh` is the one that asks for tier 2. So every command a user can start is a
process the system can *kill* rather than one it can only ask to stop — which is what tier 3 buys
and the only thing it buys (§4.2).

Nothing in C++ moved for it. The tier is a `u32` in a binary's `braam` custom section and §4.3's
promise is that the same binary runs at either, so this change is an argument to `stamp.py`, a map
in `test/run.mjs`, and the documents it made false. `bundle.bin` is the same size to the byte: the
section is fixed-width, and re-stamping rewrites a field rather than adding one.

### Why now, and why by default

§4.4 estimated a tier-3 syscall at "order 0.1 ms" and concluded that the tier had to be *"a claim
a binary makes rather than a default"*. T1 measured it at **34–44 µs** in three engines. That is
the whole argument: at a tenth of the estimate, a command that reads a file or filters a stream
pays a few hundred microseconds for isolation it cannot be talked out of, and the only program in
the system that cannot afford it is the one being typed into — a keystroke costs six syscalls, and
T1 measured sustained typing at 2.7 ms a key in Gecko and 5.1 in WebKit with the shell at tier 3.
So the default inverted and the shell is the exception, rather than the other way about.

The default lives in `cmake/BraamProgram.cmake`, which is the recipe the SDK installs, so an
out-of-tree program gets what the system's own programs get. T3 had said to leave that until T8 on
the grounds that the SDK's default is a claim about what a *stranger's* program should ask for —
and it is, which is why it should be the same claim. A program that wants tier 2 says `TIER 2` and
gives up the kill; `src/cmd/CMakeLists.txt` has exactly one such line, for `sh`.

`src/cmd/CMakeLists.txt` lost its per-program tier loop with it. An undefined `BRAAM_BIN_TIER_<p>`
now expands to nothing and the recipe's default applies, which is how `BRAAM_BIN_LIB_<p>` had
always worked.

### What the tests had to learn

The driver pumps both tiers uniformly, so most of `test/run.mjs` held unedited — including the
cases that matter most: a tier-3 `timeout` supervising a tier-3 `spin`, `watch` piping to a child,
and `^C` down a chain of two workers. Four things did not.

**The pool counts changed, and what they mean is worth more than the numbers.** `pooled()` is 1
after the first `spin 1` and 2 before `timeout 20 spin`, where before it was the two hired at boot,
untouched. The rule they now assert is that the pool grows only for a pipeline wider than what is
idle and shrinks only when a process is *killed*, so each number is hires less terminations — 8
less 7, and 13 less 11 — and is a running record of what the session has killed. They stay exact
literals; an "at least one" would have stopped testing the pool.

**`net.hold()` had to learn to count.** It held "the next process to bind a worker", which was
unambiguous while two programs took one. It is not now: a `submit("clear", …)` between the hold and
the command takes it instead — `clear` is a program, not a builtin — and in `timeout 20 spin` the
parent binds its own worker before the child that never returns. `net.hold(n)` holds the *n*-th
bind from there. The alternative was putting the binary's path into the `bind` message so a test
could hold by name, which is a change to the host protocol for a test's convenience.

**The broken-worker case was clearing the screen with the broken worker.** `clear` was the first
`exec` after `dropWorkers()`, so it crashed in `spin`'s place and gave the tier up before the case
under test ran. The clear happens before the tier is broken now.

**The two cases that give the tier up moved to the end of the run.** Neither can be undone —
`workers` stays false for the session, by design — so everything after them ran at tier 2 while
shipping at tier 3, and after this change that "everything" was the whole `/bin/sh`-as-a-program
section: a shell process spawning pipelines, `less` claiming the screen and losing it to `^C`. They
now sit just before the final `exit 7`, and that section runs at the tier it ships at.

### What this does not fix, and what it costs

The two §4.3 fidelity losses stop being true of two programs and become true of all of them. A
binary that will not instantiate reads as a crash (132) rather than as a refusal (126), because a
tier-3 instance is created inside its worker — so a spawn that runs out of memory on a loaded page
reports a crash. And `Sys::Now` is relative: nothing calls `proc_now()` today, so that is a
constraint on what may be written next rather than a regression.

Bulk I/O is where the cost lands: a syscall per `SYS_CHUNK`, which is 512 bytes, at 34–44 µs each.
T5 is the re-measurement that decides whether T6 — a bigger chunk, or batched replies — is worth
starting. It has since been taken, and the answer is no; the note above this one has it.

The shell is untouched and stays at tier 2. T7 is the design question that has to be answered
before it moves — a tier-3 shell dies with the workers, and nothing re-execs init.

---

## A pool for a system where every command needs a worker

The plan's T2. The pool in `web/proc.js` was sized for the two tier-3 binaries that
exist — `MAX_IDLE` 2, one worker hired at boot — and T1 measured what that costs the moment a
pipeline is at the tier: `cat /bin/sh | cat | wc` **hires one worker and terminates one on every
run**, in all three engines, while reusing two. Three stages want three workers at once and the
pool holds two, so the third is bought and thrown away each time. `MAX_IDLE` is now 4 and two
workers are hired at boot.

### The number is a pipeline, and the cap stays a cap

The pool only ever fills by *returning* what it hired on demand, so it self-tunes up to the peak
concurrency a session reaches and `MAX_IDLE` is the point where it stops. Four is a four-stage
pipeline's worth held with nothing running — one more than the three stages T1 measured, and once
the shell is a tier-3 process (T8) it holds one of its own besides, outside the pool.

`pool()`'s terminate-vs-keep rule was the other thing T2 asked to re-decide, and it keeps its
shape. An unbounded pool is the obvious alternative and is wrong for the reason the cap was
written: a twenty-stage pipeline would leave twenty threads behind for a session that will never
want them again. What was wrong was the number, not the rule.

The pre-hire is two rather than four because it is only about the *first* command: a `Worker`
constructor returns before its script has loaded, so what a pre-hire buys is that the load has
finished by the time something is bound into it. Two is the shell's and the first command's, and
the pool reaches four by itself on the first pipeline that wants four.

### An idle worker was holding a dead process's memory

`MAX_IDLE` 4 made a comment worth checking, and it was false at tier 3. `serveProc` dropped its
instance at the *next* bind, so a pooled worker pinned the finished process's `WebAssembly.Memory`
— as much as `PROC_MAX_PAGES`, 16 MB — until it was hired again, and four idle workers would have
pinned four of them. The instance and its memory are now released on the step that ends the
process, in `serveProc.step` where the trap path already did it, so it is one place and both tiers.
At tier 2 it changes nothing observable: `retire()` drops the whole server a moment later.

### The probe's question is about the script, not about what is running

`broke()` ended with `if (!idle.length && !procs.size) workers = false`, which is how a host whose
`procworker.js` will not load was supposed to stop trying. It cannot fire once a tier-3 process is
permanent: with the shell at the tier, `procs` is never empty again, and every `exec` would hire
another worker that will never load.

The replacement asks the question the latch was really asking. Each link records whether it
announced itself — the `{ k: "ready" }` message `deliver()` had been dropping on the floor, whose
comment already said it "says the worker loaded, and nothing more" — and `broke()` gives the tier
up only for a worker that broke before saying so. One that had loaded and then threw is a process
that crashed, which is its own business and no reason to disable the tier for the session. The
distinction is exact rather than heuristic: `onerror` before `ready` *is* a script that would not
evaluate, and the ordering holds because `ready` is posted during evaluation.

`test/fakeworker.mjs` gained the switch that models it — `net.broken` makes a link that serves
nothing and reports an error where a real one would — because nothing in the suite had ever called
`link.onerror`, and a latch with no test is a latch that was wrong twice.

### What was deliberately not done here

T1's other finding, that tier 2's bulk cost is mostly the every-64th `setTimeout(drain, 0)` in
`web/worker.js` rather than the call itself, is untouched. It is a change to how the kernel worker
yields and it wants its own measurement; sizing the pool does not depend on it.

## What a syscall costs, measured

The plan proposes moving every program to tier 3, and its first item, T1, is the gate:
the whole argument for the change is the cost of a syscall, and nobody had measured one.
Concept.md §4.4 asserts *"two `postMessage` hops and two copies, order 0.1 ms"* and the M9 note
repeats it, but both were estimates — `test/run.mjs` counts ticks rather than wall time, and its
links have no thread in them. The answer is **34–44 µs** for a tier-3 round trip and **0.2–2.9 ms**
added to a keystroke, which is two to three times better than the guess in one place and worse than
it looks in another. The figures are at the top of this file; this is why they are the ones that
were taken.

### The measurement is a difference, not a stopwatch

Timing one syscall would have needed a boundary to argue about — does the span start when the
process calls `sys_async`, or when the kernel's job is scheduled? — and it could not have been read
anyway: without cross-origin isolation `performance.now()` steps in 0.1 ms under Blink and a whole
millisecond under Gecko and WebKit, and `PROC_TASKS` is 8, so several round trips overlap and their
mean is not a per-call cost.

So the number is ΔT/ΔN over two runs of the *same command*: `wc` over one file against `wc` over
eight. Same binary, same spawn, same compile-cache hit, same instantiate, same exit, same prompt
redraw — all of it subtracts out, and what is left is the marginal cost of a round trip. ΔN is read
out of counters rather than predicted, which is what makes it a measurement and not arithmetic
about `SYS_CHUNK`.

The eight files have to be eight *different* files. `wc /bin/sh /bin/sh` is `Err(Perm)`: §5.2 gives
one open per path system-wide, because an OPFS sync access handle takes an exclusive lock and the
open-file table enforces that for every backend rather than only the one it came from. That cost
half an hour and is worth writing down.

### The tier-2 number is mostly a timer

The result that changes what T2 should do first: **tier 2's marginal cost is not a call.** Every
64th tier-2 step is scheduled with `setTimeout(drain, 0)` rather than a microtask — `web/worker.js`
does that so a process in a tight syscall loop cannot chain microtasks without the worker ever
painting — and one of those waits 1.3 ms in Blink, 2.4 ms in Gecko, 0.25 ms in WebKit. `wc` over
eight files takes that route eight times, which is 10.2 ms of a 16.2 ms command in Blink and 19 of
33 in Gecko. Discount it and a tier-2 round trip is 2–17 µs.

That is why the tiers look nearly equal in the raw figures, and in Gecko it is why tier 3 measures
*faster* than tier 2 for bulk I/O. The every-64th rule is a cheaper thing to reconsider than T6's
`SYS_CHUNK` or a batched step protocol, and it was invisible until something counted the two routes
separately.

### A keystroke has two answers, and only one of them is reassuring

A repaint is four syscalls, and the first of them damages the grid — so key-to-first-paint and
key-to-finished-echo are different numbers. Under Blink they are within a factor of four; under
Gecko the second is thirty times the first. Reporting either alone would have been misleading, so
the harness reports both, and the headline is the echo.

One key is inside a frame at either tier in every engine. **Sustained typing is not**: 64 keys back
to back cost 0.34 ms each at tier 3 in Blink but 2.7 in Gecko and 5.1 in WebKit, against 0.10 to
0.42 at tier 2. Nothing was dropped — the 64-slot ring held — but a shell that costs 5 ms a
keystroke while it is being typed into is the thing T7 and T8 have to answer to, and it is engine
dependent in a way the bulk figure is not.

The third arm exists to make that attributable. It stamps every binary tier 3 *except* `sh`, and
its keystroke figures match the stock archive's in all three engines: the keystroke cost is the
shell's own tier, not the programs'.

### Why a re-stamped twin rather than a rebuild

The tier is six u32s in a `braam` custom section and `stamp.py` strips a prior one before appending,
so the twin archives are the same binaries with one word changed — `cmake --build build --target
bench` copies the staging tree, re-stamps, and repacks. Nothing is recompiled, `bundle.bin` is
byte-identical, and the three CTest cases never see the twins because they take their inputs by
explicit path. A second build tree would have measured a second build.

### The counters are unconditional, and the clock is in the worker

Gating the instrumentation behind an option would have meant measuring a build nobody runs. What it
costs is a dozen integer increments on paths that already do a `postMessage`, one bounded ring of
key samples, and a `stats()` on what `makeProc` returns. `workers` is in there because it cannot be
inferred: with it false a tier-3 binary ran at tier 2, and the arm measured tier 2 twice.

The first version timed from the page and had a 14 ms floor — two poll intervals — which is larger
than most of the workloads. The fix was to stamp both ends in the worker, at the key and at the
repaint, and let the page poll only for *whether* it is over. A poll costs a message; it should not
also cost the measurement.

### What the harness had to survive

Three failures, none of them about tiers, each of which would otherwise have read as a hang:

- **A background tab throttles the timers being measured**, so the page refuses to start hidden and
  marks any pass that loses focus. Per pass, not per session: eight minutes of six page loads with a
  person at the machine will lose focus once, and that is no reason to discard the other five.
- **WebKit will not boot a second kernel promptly in the same document**, and will not boot the next
  one at all while the last one's worker still holds its OPFS handles. One arm per page load, two
  seconds after `dispose()`, and a retry that reloads the same pass.
- **An unanswered `selectall` is indistinguishable from a blank screen**, since `deselect()` posts a
  selection nobody asked for. `selectall` now carries the asker's id back and answers even with
  nothing to answer with.

`tools/bench.mjs` prints the page's progress as it arrives, which is how the last two were found:
a browser whose console nobody is reading is the normal case here, and a run that stops has to say
where.

---

## An SDK after all

"Plain clang, and no SDK" below is about the *toolchain* — that nothing is taken from a
distribution but the compiler. This is the other sense of the word, and it was missing: there
was no way to write a program for Braam without being inside this repository. The recipe that
turns `echo.cpp` into `/bin/echo` was an inline `foreach` body in `src/cmd/CMakeLists.txt`, the
include path was `${CMAKE_SOURCE_DIR}/src`, and there was no `install()` anywhere in the tree.

Nothing about the system required that. `exec_resolve` reads an image through the ordinary VFS
against the caller's working directory, and `exec_meta` accepts anything with a well-formed
stamp at the current `PROC_ABI` — so a `.wasm` that arrives at runtime, through `import` or
down a `curl`, is already a command. What was missing was only the means to produce one. `make
install` and `braam-sdk-<version>.zip` are that means, and the whole of it is headers, two
static libraries, a CMake package and `stamp.py`.

### One recipe, not a copy of it

`braam_add_program()` in `cmake/BraamProgram.cmake` is the old loop body, moved. `src/cmd/`
calls it for all thirty-two programs and the SDK installs the same file, so an out-of-tree
program is built by the same code rather than by a description of it that can fall behind. The
one thing the in-tree call site still adds is the tier map; the one thing it lost is the
per-binary size budget, removed separately.

That refactor is verifiable in a way a rewritten recipe would not be: `hello.wasm` built out of
tree against an unpacked archive is byte-identical to the same file built in tree.

### What is installed, and what is not

`braam_proc` and `braam_ui`, and no more. `braam_core`, `braam_fs`, `braam_svc` and `braam_user`
each carry a host import, and `test/run.mjs` asserts that no binary imports anything but
`env.memory`, `kernel.sys` and `kernel.sys_async` — so shipping them would offer a consumer a
link that produces a binary the kernel refuses to run.

Headers go to `include/braam/` rather than `include/`, because the directories under them are
`kernel/`, `fs/`, `proc/` and `ui/`, and those cannot be dropped into `/usr/local/include`.
Whole directories are installed rather than the include closure of `proc/io.h`: the closure is
a list, and a list would fall behind. Layout inside is load bearing — an include within a
directory is a bare name — so the four keep their shape and the SDK's single `-I` is
`include/braam`.

The paths are fixed rather than `GNUInstallDirs`, because the same tree is zipped and unpacked
anywhere and the package finds itself by walking up from `lib/cmake/braam`; a distribution that
said `lib64` would break that walk for no gain.

### Three things that only bite out of tree

The interesting part of the work was not the `install()` rules; it was discovering what the
project had been supplying to its own targets without saying so.

**The C++ standard.** `CMAKE_CXX_STANDARD 20` is a project setting, so a consumer got the
compiler's default and failed inside `task.h`, where `Task` is a coroutine type. It is now
`target_compile_features(braam_flags INTERFACE cxx_std_20)`: a property of the headers, which
is what it always was.

**The build type.** Freestanding, an unoptimised build does not link — `__builtin_strlen` on a
literal and an outlined `memcpy` become calls to functions nothing provides. In tree that was
invisible because the root defaulted `CMAKE_BUILD_TYPE` to `MinSizeRel`. The default moved into
the toolchain file, where it applies to everyone who uses the toolchain, and it is a `CACHE`
entry without `FORCE` so `-DCMAKE_BUILD_TYPE=` from the command line still wins.

**`-Werror`.** Inherited from an installed interface it is hostile: a consumer's code would be
held to this tree's warning policy by a library it linked. It is now
`$<BUILD_INTERFACE:-Werror>`, so the tree stays warning-clean and the SDK stays polite.

There is a fourth, smaller: `find_package(braam)` cannot search under
`CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY` with no root path. Rather than invent a root, the
installed toolchain file sets `braam_DIR` to its own directory when `braamConfig.cmake` sits
beside it — which is true of the installed copy and false of the one in the source tree, so one
file serves both.

### Forgetting the toolchain file

The one thing a consumer must supply is the toolchain, and it cannot be supplied late: CMake
picks the compiler at the first `project()`, so a build tree configured without it holds the
host's `c++` and there is no flag that repairs it. What that looked like was a page of errors
from the headers — `sizeof(usize) == 4, "wasm32"`, then `unknown type name '__externref_t'` —
which name the symptom and not the cause.

`braamConfig.cmake` now refuses a build tree whose processor is not `wasm32`, and says which
compiler it found, that the directory has to be deleted rather than fixed, and the exact
`cmake -B build --toolchain <path>` to run — with the path filled in, since the config knows
where it is. The same line is printed by `make install` at the end. Detecting it is all that is
possible: a package config runs after `project()`, so by the time it is read the compiler has
been chosen and the only useful thing left is a sentence.

The check mirrors the guard at the top of the root `CMakeLists.txt`, which has refused the same
mistake for the project's own build since M0.

### `stamp.py --sysabi`

The stamp carries the process ABI, and `stamp.py` reads `PROC_ABI` out of `sysabi.h` rather
than restating it — that is what makes a stale binary say `built for another process ABI`
instead of crashing. The installed copy has no `src/` tree to read, so the header is named on
the command line and the recipe always passes it. The alternative, baking the number into the
installed script, would have reintroduced exactly the copy the original design avoided.

### An example that cannot rot

`examples/hello/` is installed as the worked example *and* built by the ordinary build, from
one `CMakeLists.txt` that finds the SDK only if the targets are not already defined. A sample
that is not compiled is a sample that is wrong within a few commits.

What this does not do is prove the *installed* tree is complete — that would take a test that
installs to a temporary prefix and builds against it, which is a nested configure on every
`make run`. The judgement was that the example's in-tree build catches source rot, which is
frequent, and that install rules change rarely enough to check by hand at release time.

### Two archives

`make release` now emits `braam-sdk-<version>.zip` beside the site's. A directory inside the
site archive was the alternative and was rejected: the site is what a web root serves, and an
install tree is not part of it. The SDK archive is staged by installing into `build/sdk`, so
the zip and `make install` cannot disagree about what the SDK is, and `release.py` grew only
`--name` and `--require` to describe a tree that is not a site.

## No budget for a program

The per-binary lines in `tools/size_budget.txt` are gone, along with the POST_BUILD check
behind them. What they were for was making §4.4's duplication visible — every binary carries
its own allocator, string types and coroutine runtime — but `bundle.bin` already shows that in
one number, and it is the number that matters, since the archive is awaited before the first
prompt. (That number is the staging tree's now: see "One store, and rootfs.zip".) Thirty-two limits underneath it caught nothing the archive's own limit would not, and
each was a line to edit whenever a program legitimately grew.

Two entries remain, `kernel.wasm` and `bundle.bin`. CI still reports every binary's size into
the job summary, now with `--report`, under which a file with no budget is printed rather than
refused: measured, not bounded.

## Plain clang, and no SDK

wasi-sdk is gone from the build. It was never used as an SDK — §3.1 has said from M0 that the
toolchain is a compiler and nothing else, `-nostdlib -nostdinc++` being the whole of the
relationship — so what it actually contributed was a pinned tarball for CI, downloaded and
cached on every run, and a config file we had to pass `--no-default-config` to suppress. That is
a hundred megabytes and a cache key to obtain a clang that Debian ships as `clang`.

CI now runs `apt-get install clang lld llvm clang-format` and builds with no configure flags at
all: the toolchain file finds the tools itself. The pin was worth something — a tarball cannot
drift under an upgrade the way a rolling keg can — but what it was insuring against is a
compiler that miscompiles a freestanding wasm module, and against that a version the
distribution tests is at least as good a bet as one nobody else builds this way.

The toolchain file stopped composing paths out of a prefix and started calling `find_program`
for each of clang, clang++, llvm-ar, llvm-ranlib and wasm-ld, so `BRAAM_LLVM` became a hint
rather than the answer. Homebrew is why the hint survives at all: its llvm keg is not on PATH,
so `/usr/local/opt/llvm` and `/opt/homebrew/opt/llvm` are still probed first, while its `lld`
is, which is exactly the split the old code special-cased for `wasm-ld` alone. Every tool is
now checked the same way and a missing one is named at configure time. `BRAAM_WASI_SDK`, the
alias kept for a rename two milestones ago, went with it.

The formatting check left CI altogether, because it turned out to be a check on the *version* of
clang-format as much as on the tree: Debian's is older than Homebrew's, and three places in
`curl.cpp` and `main.cpp` were formatted the way only the newer one formats them. `.clang-format`
is still authoritative and those three are now written the way both versions agree on — one of
them a line over the 100-column limit that the newer clang-format declines to break — but which
clang-format a contributor happens to have is no longer a way to fail the build.

`--no-default-config` stays. Its justification was wasi-sdk's `clang++.cfg`, but any
distribution may ship one, and a sysroot injected behind the flags is exactly the failure the
flag makes impossible.

### The wasm features are named now

The first CI run on Debian's clang 18 failed on `unknown type name '__externref_t'`, which is
what clang says when reference-types is off. It had never been off: every clang the build had
seen was new enough to have it in the default CPU. That default is not a stable thing to build
on — the feature set `generic` implies has moved twice in recent releases — so the toolchain file
now names what it needs, `-mreference-types -mbulk-memory -msign-ext -mmutable-globals
-mnontrapping-fptoint`, and the code compiles the same on clang 18 as on clang 22.

Two of those are load-bearing rather than cosmetic. Without reference-types the externref table
of §3.7 cannot be declared at all. Without bulk-memory, LLVM stops lowering `memcpy` and
`memset` inline and emits calls instead, which is a link error rather than a silent libc
dependency — correct behaviour, thanks to `--allow-undefined` being gone, but a puzzling one to
debug.

The list was checked by building with `-mcpu=mvp` in front of it: nothing in the tree needs a
feature that is enabled only by a modern default. `kernel.wasm` came out 12 bytes smaller that
way, which is the measure of how little the rest of the default set is worth here.

## The Linux console's sixteen colours

`web/render.js` shipped a palette of nobody's in particular — a softened set picked to look
pleasant against its own background. It is the Linux console's now: `#AA1111` red, `#11AA11`
green, `#AA5511` for the brown that a 16-colour palette calls yellow, `#AAAAAA` for white, and
the `#55` / `#FF` bright half above them.

The reason is that the indices are the interface. §2.3 puts a palette index in the cell rather
than a colour, so `COLOR_YELLOW` in a program's `style_set` means whatever the page decides — and
a program written against a terminal has forty years of expectation about what index 3 looks
like. A palette that renders 3 as a pale gold rather than a brown makes `COLOR_YELLOW` a
surprise, and the tests, which read indices, cannot see the difference. Matching the console
removes the question.

The one departure is the floor: the console's `#00` channel is `#11` here, so black is `#111111`
and the dark half is mixed from `11` rather than `00`. A canvas is not a CRT and true black on a
backlit panel reads as a hole in the page rather than as the absence of light. It is the one
value `web/index.html` was already built around, so the page's background is unchanged and only
its text moved, to `#AAAAAA` — index 7 — with the error line on bright red, so the frame and the
grid are the same terminal rather than two nearly-equal greys.

`web/embed.html`'s amber palette is untouched: it exists to show that a palette is an embedder's
choice, and it is the only thing proving the `options.palette` path still works.

---

## A prompt that says where you are

The prompt now names the working directory: the basename, plain white on blue, then a space and
the bright white `$`.

```
home $ _
```

It cost no operation. `Sys::Style` was already there for the red `[N]`, and `Sys::Chdir` already
answers "where am I" — the shell asks the same question `pwd` does, through the same call. The
change is a third run in `anchor` and a third field in `Prompt`, and the wasm ABI, the import
list, the export list and the §4.3 table are all exactly as they were.

**The basename, not the path.** The terminal is 80 columns at its widest and a prompt that grows
with the depth of the tree eats the line being typed — and the line editor anchors on wherever
the prompt ended, so a long one is not wrong, only cramped. The basename is what actually answers
the question a prompt is asked. `path_basename` already returns `/` for the root, which is the
one case where a bare name would have been empty, so the root reads `/ $` with no special case
written for it.

**Asked every prompt, not remembered.** The obvious optimisation is for `cd` to stash what
`cwd_set` hands back — it already gets the resulting absolute path and throws it away. It was not
done. `cd` being the only thing that moves the shell is true today and is not a property anything
enforces; a cached prompt would go quietly stale the first time something else moved it, and
staleness in a prompt is worse than a syscall, because a wrong prompt is believed. One
`Sys::Chdir` per *line* is a park and a step against the six ticks a single keystroke already
costs (§4.4), so it is not on any path that matters.

**The space is outside the colour.** Three runs, not two: the directory on blue, then `" $ "` on
black. Putting the space inside the coloured run would have been one fewer `String` and a blue
block that runs one cell past the name, which reads as a trailing space in a highlight rather
than as a separator. So the separator lives at the head of the bright white run, and the shell
picks `"$ "` or `" $ "` depending on whether there is a directory to separate from — which is
also the whole of the fallback: a `Sys::Chdir` that fails leaves the prompt exactly as it was
before this change rather than leaving a hole where a name should be.

**Two things about it are lifetime, not taste.** `Prompt` holds non-owning `Str`s and always has;
until now every one of them pointed at a literal, and `dir` points into a `String` the loop body
owns. That `String` must outlive the `read_line` that draws it — including the `^L` path, which
re-runs `anchor` with the same `Prompt` — which is why it is declared in the loop body and not
built into the call. `path_basename` returns a `Str` *into its argument*, so the two are one
lifetime, not two.

The second is a trap worth naming: `dir.empty() ? "$ " : " $ "` does not compile against a
freestanding target. `Str`'s `const char *` constructor is `__builtin_strlen`, which folds for a
literal and does not fold for a pointer chosen at run time — so the ternary emits a call to
`strlen`, and `--allow-undefined` is deliberately absent (§C.3), so the link fails rather than
the program. `"$ "_s` is the fix: the `operator""_s` in `str.h` is the sized constructor, and the
choice happens between two finished `Str`s.

**The smoke test now builds the prompt rather than spelling it.** Thirty-odd assertions compared
the cursor row against `"$"` or `"[130] $"`, and the suite `cd`s half a dozen times, so the
expected text is no longer constant. It tracks the directory it is in and a `prompt(status)` helper
composes what should be on the row — which is a stronger assertion than the literal was, since it
now also says the shell is where the test thinks it is. The colour checks got the same treatment:
they were "column 0 is bright white", and they are now "the name is white on blue, the space
after it is not, and the `$` is bright white", which is the thing the change is about.

---

## Telling a CORS refusal from a dead network

M6 gave `curl` one hint on `Error::Io` — "a cross-origin URL needs CORS" — because `fetch`
reports a refused origin and an unreachable server identically, as a `TypeError` with nothing in
it. The hint was right most of the time and wrong in the one case where a user most needs to be
believed: when the network really is broken, the message sent them looking at a header their
server may already send.

The two can be told apart, and the browser will do it. A `no-cors` request is not blocked — it
goes out, the server answers, and what comes back is an opaque `Response` with status `0` and no
body. Useless to read, which is why braam cannot use it for the fetch itself, and conclusive as
a probe: only a reachable server resolves it at all. `web/svc.js` runs one when, and only when,
the real fetch has already rejected, and reports `Error::Perm` if it succeeds and `Error::Io` if
it does not. The probe is aborted the moment it resolves, since `fetch` settles on the response
headers and the body would otherwise be downloaded for nothing.

`Perm` is the right existing error rather than a new one. What happened is precisely a
permission denied: the request was made, the server answered, and the browser would not let the
page read the reply. Adding an `Error::Cors` would have put a browser's vocabulary into a kernel
enum that storage and the filesystem share, for a distinction one program makes — and it would
have been an ABI change in `abi.js` and `result.h` for a diagnostic.

So `curl` now says "response is not accessible because the server did not grant cross-origin
access" for one and "no answer" for the other, and neither can be mistaken for the other's
cause. The first says what happened rather than naming the header, at the cost of wrapping on a
60-column grid; the second is short because there is nothing more to say.

The fake carries the same distinction: a route in `test/fakesvc.mjs` may name the error it fails
with, so `test/run.mjs` drives both paths without a network. What it cannot cover is the probe
itself — there is no CORS in Node and `mode` is ignored there, so the fake asserts what the
kernel and `curl` do with each answer, not how `web/svc.js` arrives at it. That is the same gap
every service has against a fake, and the same reason `tools/wsd.mjs` exists for the one where
it mattered more.

None of this makes a cross-origin fetch work. It cannot: the same-origin policy is what stops a
page reading a reply that carries the user's cookies, and no flag, API or permission prompt
lifts it. A relay would, at the cost of the server braam is built not to need. The change is to
the diagnosis alone, which is the part that was fixable.

## Paste, as typing

`Cmd+V` — `Ctrl+V` where that is the chord — puts the clipboard into the terminal. It cost one
function in `web/keys.js`, a feeder in `web/worker.js`, four lines in `web/braam.js` and a return
value on `key()`. Nothing else moved: no import, no export added, no syscall, and no operation in
the §4.3 table.

**A paste is keystrokes.** §2.3 leaves no alternative — the terminal is a cell grid and the
keyboard is a `Channel<Key>`, so there is no stream for pasted text to be written into and no
second way in for it to take. `pasted(text)` turns the text into the run of key codes that would
have typed it: `Enter` for a newline however the platform spells it (`\r\n` and `\r` are one
`Enter`, not two), `Tab` for a tab, and nothing for any other control character, because there is
no key that produces one. The reward for that translation is that everything downstream already
works — the line editor, a cooked reader like `cat`, and a raw claimant like `edit` each get a
paste on the terms they already read keys on, and none of them can tell it from fast typing.

**Why `key()` now returns something.** The keyboard ring holds 64 keystrokes and drops what it
cannot hold — the right policy for a keyboard, and the wrong one for a paste, which is routinely
longer than the ring and would arrive with its tail cut off. The host has to feed the run at the
rate the console drains it, and to do that it has to know when the ring is full. Nothing else
tells it: occupancy is not in the `Screen` descriptor, `tick`'s delay says nothing about it, and
a fixed chunk size per turn is a guess that is silently wrong when a raw claimant is slow to
read.

So `key()` reports whether it queued the keystroke, and `web/worker.js` pushes until it is
refused, ticks, and comes back on a later turn for the rest. This is the smallest change that
makes the loss impossible rather than unlikely, and it does not weaken §2.2: the return value is
a fact about the call the host just made, not a result arriving from the kernel — no data crosses
and nothing is scheduled. The alternatives were worse in kind, not degree. A `paste(ptr, len)`
export would be a seventh entry on the boundary and a byte stream into the keyboard, which is the
invariant inverted. Growing the ring to fit the largest plausible paste is a number that is
always too small or always too large, and it would still drop the paste after it.

A second paste while one is being fed joins the queue rather than displacing it, for the reason
`pbpaste`'s superseded waiter answers empty: a run that is silently truncated in the middle is
worse than one that arrives late. A key *typed* during a paste goes straight in ahead of the rest
of the run — `^C` must not wait behind a hundred queued keystrokes.

**Who gets the gesture.** Neither `Ctrl+V` nor `Cmd+V` is prevented, which is deliberate and was
already true: the `paste` event the browser makes from them is the only way a page may read the
clipboard with no permission, which is what `pbpaste` is built on (§5.4). The event is the
document's rather than the canvas's, so the focus decides first — an embedded terminal must not
swallow a paste meant for a field beside it. That gate is new for `pbpaste` too, and it is a
fix rather than a side effect: its waiter was global, so once a paste could be typed, an
unfocused terminal with `pbpaste` running would have taken one meant for the terminal next to it.
Within the focused terminal a waiting `pbpaste` wins and nothing is typed — it asked for exactly
this gesture, and a program reading the clipboard wants the text rather than the keystrokes.

**What it does not fix.** A multi-line paste at the prompt loses everything after the first
command's newline, and that is type-ahead across a command boundary rather than anything about
pasting. The shell claims the raw route to edit a line and gives it back around anything it runs
(§3.5), so keystrokes arriving while a command runs are *cooked* — echoed, and put in the console
channel as that command's stdin, which is what makes `cat` read what is typed. The shell's line
editor never reads that channel, and `console_fg_set` clears it when the next command is armed.
Handing the leftover cooked input to the editor when it retakes the keys is the fix, and it is a
change to the console discipline rather than to the paste: a non-blocking read of the console
channel, and a rule for what a partial line means when the claim moves. Pasting one line — a
command, a path, a URL — is the case that motivated this and it is exact.

`test/run.mjs` feeds a pasted line longer than the ring with the same loop `web/worker.js` uses
and asserts both that the whole run arrives and that it took more than one turn, so a ring that
silently grew or a `key()` that stopped reporting would show up as a test that no longer proves
anything.

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
Little of it was the applet retirement — that change carried its own notes, and the milestone
record already said how to read every milestone under it. The stale things
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

## The milestones, and the criteria they were accepted against

The ten notes that follow are the *why* of M0–M9, one per milestone, written as each landed.
`doc/Milestones.md` was the plan they were written against — one objective and a handful of
acceptance criteria apiece, with a short note on how each milestone departed from its plan. Every
one of those departure notes is stated at length below, and the arithmetic it carried (the size
trajectory, the program counts) is in the notes too, so what only it held was **the objectives and
the criteria**. Those are not history: a criterion is a standing behavioural contract, and a change
that breaks one is a regression however green the three CTest cases are. So they are here, and the
plan is deleted.

Twenty-two criteria, M0 to M9:

- **M0 — Nucleus.** Freestanding build, the coroutine shim, the allocator, `Str`/`Vec`, `host_log`,
  and a size budget from the first commit.
  - `make` produces a wasm binary with the Appendix C command line.
  - A static page loads a 4 KB wasm and logs a line to the console.
  - Size budget recorded (32 KiB) and enforced by CI.
- **M1 — Scheduler.** `Task<T>`, ready queue, wake tokens, `tick()`, `sleep_ms`, with `CancelToken`
  in every awaitable from here on.
  - Two coroutines interleave sleeps in the correct order.
  - Cancelling a sleeping task unwinds it and runs its destructors.
- **M2 — Screen and keys.** Cell grid, canvas renderer, damage rectangles, `Channel<Key>`,
  `OffscreenCanvas` transfer.
  - Typed characters appear on screen and the cursor moves.
  - Window resize reflows and `resize(cols, rows)` reaches the kernel.
- **M3 — Userland shell.** `LineEditor` with history and editing, tokeniser, program registry,
  argv, exit codes.
  - `echo hello` prints, `help` lists the programs.
  - Up-arrow recalls history; a nonzero exit code is observable.
- **M4 — Streams.** Stdio as channels, pipes, redirection, cancellation on `^C`.
  - `ls | grep foo` works.
  - `^C` interrupts a running pipeline and returns a prompt.
- **M5 — Filesystem.** Mount table, `MemFs`, `BundleFs` from a fetched archive, `OpfsFs` with the
  open-file table.
  - Write a file, reload the page, the file is still there.
  - `df` reports quota, usage, and ~~persistent versus best-effort mode~~ — the mode is on the
    boot banner now, since it is a property of the origin and not of a mount. **Amended**, not
    retired: see "df is a table".
  - ~~With OPFS unavailable, the system boots on `MemFs` and says so.~~ **Retired**, not broken:
    there is no second store to boot on, and one that loses everything at the reload is the
    failure the criterion was written against. See "One store, and rootfs.zip".
- **M6 — Host services.** `fetch`, timers, WebSocket, clipboard, the `externref` table and `JsRef`.
  - A `curl`-ish command fetches a URL and prints the body.
  - A chat client works over a WebSocket.
  - `/mnt/import` and `export` move files in and out.
- **M7 — Depth.** A layout layer over the cell grid, job control, `/proc`-style introspection, an
  embedding API for host pages.
  - A full-screen editor opens, edits, and saves a file.
  - Jobs can be backgrounded and listed.
- **M8 — Isolated processes.** The §4.3 ABI, a `WebAssembly.Instance` per process, per-pid import
  closures, memory caps, a module cache, cross-boundary copies.
  - A program runs as its own instance with a 16 MB cap, and `memory.grow` fails past it.
  - A process cannot issue a syscall on behalf of another pid.
  - ~~Tier selection comes from binary metadata; userland behaviour is unchanged.~~ **Retired**,
    not broken: there is no selection left to come from anywhere. See "Tier 2 is deleted".
- **M9 — Liveness isolation.** A worker per process, `worker.terminate()` as `SIGKILL`, module
  `postMessage`.
  - `while(1){}` in an untrusted program is killable without reloading the page.
  - The shell stays responsive while such a program runs.

**Two changes since reach back through the whole list, and neither cost a criterion.** The kernel
applet and the program registry are gone, so where a criterion says a program was registered that
program is a binary in `/bin` and `help` and `ls` read a filesystem — the criteria are about what
the system *does*, not where the code lives, and the ones that named `echo` and `sleep` as applets
are now met by binaries and checked by `test/run.mjs` rather than by the in-wasm suite. And the
tiers are gone, which retires exactly one criterion, M8's third, as above.

**Where they are checked.** M0's budget is the `size` case and M1's pair is checked twice — in
`tests.wasm` against a fake clock and in `smoke` against the shipping kernel. M5's persistence is
mechanical too: `smoke` writes a file, throws the instance away, builds a new one against the same
JS-side store and reads it back. Most of the rest are shell-level and much of what is scriptable is
driven by `test/run.mjs`; what is left — a resize reflowing, a chat between two tabs, a picker
moving a file in — is checked by hand at the prompt, which is what a change touching one of them
still owes.

---

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

*Superseded.* The archive is unpacked into the store rather than mounted from memory, so the
kernel does not read it at all and the hand-rolled format bought nothing — it is `rootfs.zip`
now, and only `web/fs.js` parses it. See "One store, and rootfs.zip". The last sentence still
holds: the smoke test is still given the archive the build just produced.

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

*Superseded in part.* The ordering is unchanged and the reason for it stands, but the line above
the first prompt is now a refusal rather than a warning, and there is no prompt after it. See
"One store, and rootfs.zip".

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

M4's objective says `Channel<Bytes>` and `Bytes` already exists — `using Bytes = Span<const u8>` —
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

Interrupting a *running* program is M4's criterion and stays there. It needs the
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

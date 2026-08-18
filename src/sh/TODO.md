# Scripting in /bin/sh

The plan of record for turning `/bin/sh` from a prompt into a language. **What lands is deleted
from here** — its reasoning moves to [Release_Notes.md](../../doc/Release_Notes.md), which is
where the *why* lives once it is code. Four stages have landed, recorded there under *"The lexer
stopped removing quotes"* (quote removal left `Lexer::next` for a new [expand.h](expand.h)),
*"The shell got variables"* (the `$` walk, `IFS` splitting, and the `Field` flag the mark could
not carry), *"A line became a tree"* (the node arena in [parse.h](parse.h), `exec_node`/`Flow` in
[job.cpp](job.cpp), and multi-line input by re-parsing) and *"The tree learned to branch"*
(`if`, the three loops, and `break`/`continue` as builtins that ask). What remains is numbered
below, once, in the order it should be done.

Stages are **S1**…**S10**. A number is a name, not a position: when a stage lands its section is
deleted and its number retires with it, so the rest keep the numbers they have and a commit
message or a Release_Notes heading naming S4 still means the same work a year later.

## Context

`/bin/sh` today is a prompt, not a language. Its grammar is one line:

```
pipeline := command ('|' command)* ['&']
command  := (word | redirect)+
redirect := '<' word | '>' word | '>>' word | '2>' word | '2>>' word
```

There is no globbing — `*` and `?` are ordinary word characters — and no way to run a file of
commands beyond `sh -s` reading stdin. Release_Notes.md records this as an explicit
*non-*decision:

> `/bin/sh` has no variables, no `-c`, no globbing and no scripts beyond `sh -s`. None of that
> was blocked by the shell being kernel code and none of it is blocked now; they were simply
> never written.

The target is the full v7 Bourne subset — lists, `if`/`while`/`until`/`for`/`case`, functions,
the `${x-y}` family, positional parameters, command substitution, here-documents, globbing,
`trap`, and `test`/`[` as a builtin. The reference is the same author's V7 port at
`/Users/vak/Project/Besm-6/v7besm/cmd/sh/` (≈3,450 lines of code across 29 files).

**Headroom is not the constraint.** `kernel.wasm` does not change — every line of this is in the
process binary. `sh.wasm` is 127,614 bytes and only `sh` links `src/sh/`; the only budget it
spends is `rootfs/ = 1048576` against a 597,092-byte tree.

**And no syscall changes.** Everything scripting needs — pipe, spawn, wait, open, list, stat,
chdir, getpid — is already in the §4.3 table. That is the concrete proof of the note above.

---

## What is genuinely impossible, and the honest substitute

Six things in the v7 set do not exist in Braam and must be decided rather than ported.

| v7 | Why not | Substitute |
|---|---|---|
| `( list )` as a real subshell | There is no `fork`. Spawning `/bin/sh` loses the variable table (there is no environment anywhere in the ABI — `Sys::Spawn`'s payload is three fds and an argv blob) and costs a worker against `SYS_PROC_DEPTH = 8`. | Run it in the current process, **saving and restoring the shell's own mutable state** around it: cwd, variables, positional parameters, traps, `$-`. Gets `(cd /x; ls)` and `(set -e; …)` right; loses only memory isolation. |
| A compound command in the background — `( … ) &`, `while … done &` | Backgrounding means the shell keeps running while the group runs, and nothing in a process can wait for a sibling task (`Channel::park_sender` panics rather than lose a wakeup). | **Refuse it**: `cannot run a compound command in the background`. `cmd &` on a simple pipeline is unaffected. |
| `exec cmd` replacing the image | A process is a wasm instance in a worker; there is no re-instantiate-in-place, and `spawn` makes a new pid. | `exec` with **no** command makes its redirections permanent on the shell — that works exactly, and is half of what `exec` is for. `exec cmd` spawns, waits, and exits with the child's status. |
| `#!` scripts | `exec_meta` in [../user/exec.cpp](../user/exec.cpp) requires `\0asm` plus a `braam` custom section with `PROC_ABI == 9`. A text file can never be exec'd. | `sh file` and `sh < file` only. No `./script.sh`. |
| `export` reaching a child | There is no environment in the wasm ABI. | `export` records the intent, honoured by `.`, functions and `eval` — same process. Taking the name cost `/bin/export` its own, which is now `save`. Nothing crosses a spawn. The alternative is `PROC_ABI == 10` across `sysabi.h`, `syscall.cpp`, `exec.cpp`, `rt.h`, `web/proc.js`, System_Calls.md and `run.mjs` — a milestone of its own with no consumer, since no program in `/bin` reads an environment and there is no `PATH`, `HOME` or `TERM` to carry. |
| `trap … <signal>` | There are no signals, and `CancelState::cancelled` ([../kernel/sched.h](../kernel/sched.h)) is a **sticky** bool: once ^C sets it, every subsequent await returns `Err(Cancelled)` at once. | `trap … 0` (EXIT) works on any normal exit. `trap … 2` (INT) works in an **interactive** shell, where ^C is an ordinary key and the shell was never cancelled — it fires when a stage reports 130. In a *script* shell the process itself is cancelled, so the handler could neither spawn nor write: accepted, never fires. `trap '' INT` (ignore) is impossible outright. |

One further gap this work **creates** and must record:

- **`$$` is not unique per shell.** `/bin/sh` at the top level takes init's pid, since it is a
  process inside init's task (Concept.md §4). A nested `sh` gets its own.

Left out deliberately: `$((…))` arithmetic, `${x:-y}` colon forms, `${#x}`, `${x#…}` (none are
v7); `set -o`; `ulimit`, `umask`, `newgrp`, `hash`, `times` (no kernel concept exists); `bg` and
`^Z` (a standing gap for a stated reason); a `PATH` (a variable nothing would read).

---

## Stages

Ten, in order. Each builds, passes CTest, and leaves a shell strictly better than the one
before. Each carries the invariant it protects — that part is not recoverable from the code, and
moves to Release_Notes.md when the stage lands and this section is deleted.

| # | Stage | Days |
|---|---|---|
| S4 | Globbing and `case` | 2.0 |
| S5 | Command substitution | 2.0 |
| S6 | Functions, `.`, `eval`, `return` | 2.0 |
| S7 | Redirection completion | 2.5 |
| S8 | The scripting builtins | 2.0 |
| S9 | Entry points | 1.0 |
| S10 | Integration, docs, budget | 2.0 |

**13.5 days, call it 16 — three working weeks.** The two places it will go over are the
capture/deadlock reasoning in S5 and the here-doc/fd bookkeeping in S7. A minimum-credible slice
(`test` and `[` out of S8, plus `sh file` out of S9) is **~3 days** and is most of the remaining
utility.

### S4. Globbing and `case`

**Scope.** `*`, `?`, `[a-z]`, `[!…]` over the real store, and `case`/`esac` with its arms. This
is the first reader the quoting mask has, and what pays for it.

**Globbing splits pure from impure.**

- **`match.cpp` — pure, and added to `tests.wasm`.**
  `bool glob_match(Str pattern, Span<const u8> mark, Str name)` with `*`, `?`, `[a-z]`, `[!…]`; a
  marked metacharacter matches itself. v7's `gmatch`, with its tail calls turned into loop
  iterations (the port's own fix — an unbounded pattern recursed once per character).
- **The directory walk lives in `expand.cpp`** — split the field on `/`, expand only components
  holding an unquoted metacharacter, `list_dir` each, sort the survivors, and leave the word alone
  if nothing matched, which is the Bourne rule. There is no shared sort in the tree (`ls` has its
  own `sort_block`), so a short insertion sort.

**`case` patterns use the same `glob_match`.** That shared use is what pays for it.

**Files.** new `match.{h,cpp}`; [expand.cpp](expand.cpp) (the walk), [parse.cpp](parse.cpp) and
[job.cpp](job.cpp) (`case`), [../../test/CMakeLists.txt](../../test/CMakeLists.txt).

**Tests.** new `test_match.cpp` — `*`, `?`, `[a-z]`, `[!…]`, a quoted `*`, a leading dot, the
pathological `a*a*a*b`. `run.mjs` — `echo /bin/l*` over the real store.

**Done when** `match.cpp` is in `tests.wasm`, a quoted `*` matches itself, and `echo /bin/l*`
lists real files.

### S5. Command substitution

**Scope.** `$( )` and backticks, `ShIo::capture`, the drain protocol.

**`$( )` and backticks: a pipe the shell itself drains.** A program stage takes the write end by
`spawn`; the shell closes its copy and reads the other end to `Err(Closed)` *before* waiting the
children — the shell is the drainer, so the eight-chunk pipe never fills for good. The one hazard
is a **builtin** stage, which runs in the shell's own turn ([builtin.h](builtin.h)) and would fill
eight chunks with nobody left to drain them. One field on the existing `ShIo` fixes it:

```cpp
struct ShIo {
    u32 in  = SYS_STDIN;
    u32 out = SYS_STDOUT;
    u32 err = SYS_STDERR;
    String *capture = nullptr;   // set: a builtin appends here, not to `out`
};
```

When `capture` is set a builtin appends there instead of writing to `out`. It is **one line per
builtin** — six call sites today — precisely because `builtin.h` already requires each to buffer
its output and write it once. That discipline becomes load-bearing for a second reason, and its
paragraph must say so.

Residual hazard, pre-existing and not a regression: a builtin *upstream* of a program in the same
pipeline (`echo x | wc`) still writes into a real 8-chunk pipe with the reader not yet draining.
That is today's behaviour; do not fix it here.

**Substitution is a hook the expander calls, not a pre-pass** — v7's `copyto`/`skipto` split — so
that `${x-$(cmd)}` runs the command only when the branch is taken. The pure half of expansion
takes a `substitute`/`glob` pair alongside its name lookup; the unit tests pass canned ones, so
it stays testable without reaching a syscall.

**Files.** [builtin.h](builtin.h) and every file in `builtin/`, [expand.cpp](expand.cpp),
[job.cpp](job.cpp), [tokenize.cpp](tokenize.cpp) (the balanced `$(` scan).

**Tests.** `test_tokenize.cpp` — the balanced `$(` scan and `more` inside one. `test_expand.cpp`
— a canned `substitute` callback, including inside `${x-…}` where the branch is not taken.
`run.mjs` — `x=$(ls /bin | wc -l); echo $x`, substitution through a real pipe the shell drains.

**Done when** `$(…)` works with both a program and a builtin inside it, and the `${x-$(cmd)}`
case runs the command exactly once and only when taken.

### S6. Functions, `.`, `eval`, `return`

**Scope.** `name() { … }`, `.` (source), `eval`, `return`, `unset -f`.

**A parsed tree becomes refcounted, because a function body outlives its line.** Today a parse
result dies with the line, and that is the one assumption functions break: a function body must
outlive the line that defined it, and so must a sourced file's, since a function it defines
outlives the `.` that ran it.

So the tree gains a `u32 refs`. When a parsed line holds any `FuncDef`, `run_line` `heap_new`s the
tree instead of keeping it in the frame; each definition takes a reference, redefinition and
`unset -f` drop one, the last drop frees. The function table is `Vec<FuncEntry *>` built on first
use — [job.cpp](job.cpp)'s `g_jobs` pattern, for the same reason (a namespace-scope `Vec` has a
destructor and nothing provides `__cxa_atexit`). The variable table is the same shape: a linear
scan over tens of entries beats a `HashMap` whose keys would have to view `String`s owned
elsewhere.

`eval` is the same machinery with a lifetime of one command. `.` parses a whole file into one tree
and executes its root in the *current* `Ctx`, which is what lets a sourced file set variables and
define functions. A function is looked up **before** the builtin table and before `/bin`, which is
v7's order.

**Files.** [parse.h](parse.h), [job.cpp](job.cpp), `var.cpp`, `builtin/`.

**Tests.** `test_parse.cpp` — `fn(name){B}` and that a tree survives the line. `run.mjs` —
`f() { echo $1; }; f hi`; a sourced file that defines a function still callable after the `.`
returns.

**Done when** a function outlives its line, redefinition frees the old tree, and a function
shadows a builtin of the same name.

### S7. Redirection completion

**Scope.** `<<` and `<<-`, `>&`/`2>&1`/`>&-`, `exec` redirections, inherited base stdio, and
`( … )` as a state checkpoint.

**Here-documents: a `/tmp` file, always, with no size-based mode switch.** A pre-filled pipe has a
hard cliff at 8 × 512 = 4,096 bytes — the shell fills it and parks with no reader, and it cannot
start the reader first without giving up the ordering. A here-doc over 4 KB is not exotic.
`wipe_tmp` in [../user/boot.cpp](../user/boot.cpp) already creates and empties `/tmp` at every
boot, which is what v7's `settmp`/`rmtemp` want. The temp path is tracked in `Run` and removed in
the same sweep that closes leftover fds; a background pipeline hangs it on the `JobEntry` and
`jobs_report` removes it on reap.

**Files.** [job.cpp](job.cpp), [job.h](job.h), [parse.cpp](parse.cpp), [tokenize.cpp](tokenize.cpp).

**Tests.** `test_parse.cpp` — the new redirection forms and a pending here-doc body setting
`more`. `run.mjs` — `cat <<EOF` / body / `EOF`, then `ls /tmp` showing the temp file gone; a
here-doc over 4,096 bytes; `2>&1` merging into one stream.

**Done when** a here-doc larger than a pipe's capacity works, `/tmp` is clean after it, and
`exec >file` at the prompt persists to the next command.

### S8. The scripting builtins

**Scope.** `test`, `[`, `read`, `wait`, `trap`, `:`, and `set -e -x -u`.

**`test`, `[` and `:` become builtins — and [builtin.h](builtin.h)'s rule needs amending.**
`builtin.h` says what makes a builtin is that it touches the shell process's own state, and
`test` does not. The honest amendment is that control flow introduces a **second clause**:

> A builtin is the shell's own state **or the condition of a loop.** A program costs an
> instantiation and a worker — roughly a millisecond (Concept.md §4.4) — and a `while [ … ]` pays
> it once per iteration. A hundred-iteration loop would spend a tenth of a second and a hundred
> workers deciding it should keep going.

Deliberately narrow: it admits `test`, `[` and `:`, and does not admit `echo`, which is never a
loop's condition. Do **not** also ship `/bin/test` — nothing else could invoke it (there is no
`find -exec`), so it would be duplication for nothing. Concept.md §4's matching paragraph changes
with `builtin.h`, and the two must not drift.

`test` gets the full v7 operator set (`-r -w -x -f -d -s -t -n -z = != -eq -ne -gt -ge -lt -le !
-a -o ( )`) over `stat_of`. Two answer honestly from the "no file permissions" gap: `-w` is true
for anything on a writable mount, and `-x` is true only for a file whose first four bytes are
`\0asm` — which is what executable means here.

**Files.** new `builtin/test.cpp`, `builtin/read.cpp`, `builtin/wait.cpp`, `builtin/trap.cpp`;
[builtin.h](builtin.h) and `builtin/table.cpp`; [job.cpp](job.cpp) for `set -e -x -u`.

**Tests.** `run.mjs` — `if [ -f /share/motd ]; then echo yes; fi`, the `test` builtin and `if`
together; `set -e` stopping a list on a failure; `trap 'echo bye' 0` firing on exit.

**Done when** `while [ … ]` loops without spawning a worker per iteration, and `builtin.h` and
Concept.md §4 carry the same amended rule.

### S9. Entry points

**Scope.** `sh file args`, `sh -c cmd`, `$0`, and the exit-status conventions.

**Files.** [../cmd/sh.cpp](../cmd/sh.cpp), [shell.cpp](shell.cpp), [shell.h](shell.h).

**Tests.** `run.mjs` — plant a script in [../../test/fakefs.mjs](../../test/fakefs.mjs) before
boot (see Verification below), then `sh /home/t.sh` and assert its rows; `sh -c '…'` needs no
file at all.

**Done when** a script file runs to completion and its exit status reaches the parent shell's
`[n]`.

### S10. Integration, docs, budget

**Scope.** The `run.mjs` cases that need the whole language at once, Concept.md §4.5,
Release_Notes.md, CLAUDE.md, Programming_Manual.md, and a re-measure against
`tools/size_budget.txt`. The obligations are listed under **Documentation obligations** below.

**Done when** every obligation in that section is discharged and `size` is green without a
budget change.

### Size

Calibrated against the binary rather than guessed per stage, and re-measured twice: S1 cost
20,807 bytes over 1,017 new lines and S2 cost 10,644 over ~450 — ≈20 bytes of wasm per line, and
S2 came in under that because the node arena replaced code as much as it added. The stages left
are more coroutine than logic, so budget nearer 40.

- **`sh.wasm`: 114,936 → ~175,000 bytes.**
- **Staging tree: 599,238 → ~660,000 of 1,048,576 — 63%.** No budget change needed.
- **`kernel.wasm` does not move at all.**

~1,500 more lines of shell C++ (`src/sh/` 3,501 → ~5,000), ~700 of `test/unit/`, ~220 of
`test/run.mjs`, ~340 of documentation. The v7 reference is 3,710 lines of C for the same feature
set while doing its own memory management, `setjmp` control flow and string library.

---

## Verification

Each stage lists its own cases above. What follows holds across all of them.

**The purity boundary is what keeps this testable, so hold it.** `parse.cpp`, `tokenize.cpp` and
the new `match.cpp` touch nothing but `Str`, `String` and `Vec`, and
[../../test/CMakeLists.txt](../../test/CMakeLists.txt) compiling them into `tests.wasm` **is**
the check — a syscall in any of them is a link error. `expand.cpp` does not qualify once it can
call `list_dir` and `exec_node`, so factor its pure part — quote removal, `$` splitting, `${…}`
operators, field splitting against `IFS` — behind callbacks and test that; the callbacks are
table literals in the test.

**Unit tests are written in `test_parse.cpp`'s string-shape style**, where one rendered string
compare checks a whole input. `shape()` growing a case per construct is the whole strategy.

**A script file needs no new driver machinery**: `store.files` in
[../../test/fakefs.mjs](../../test/fakefs.mjs) is a `Map` keyed by path, so plant the script
before boot —

```js
store.files.set("/home/t.sh", enc.encode("for i in 1 2 3\ndo\n  echo $i\ndone\n"));
```

— then `submit("sh /home/t.sh")` and assert the three rows. No archive change, no `rootfs/`
growth, and the script sits next to its assertion.

**Standing checks.** `smoke` asserts the import/export surface per binary; nothing here adds an
import, so it must stay green untouched — if it goes red, something reached past `braam_proc`.
`size` should need no budget change; if it does, that is the signal to re-read the estimate
above. The M0–M9 acceptance criteria stay live; the ones this touches are M3's userland shell and
M7's depth.

---

## Documentation obligations

- **[Concept.md](../../doc/Concept.md)** has no shell-grammar section today — §4 describes the
  process model and mentions builtins, and the grammar lives only in `parse.h`'s header comment.
  Add **§4.5, "The shell's language"**, after §4.4 and before §5: the grammar in the BNF shape
  `parse.h` already uses; the expansion order (parameter → command → field splitting → globbing →
  quote removal); the six impossibility rows with their substitutes; and the amended builtin rule.
  §4.5 is a new leaf — **do not renumber anything**. Amend §4's builtin paragraph in place, and
  keep [builtin.h](builtin.h) in step with it.
- **[Release_Notes.md](../../doc/Release_Notes.md)** — a new heading per stage, appended, never by
  rewriting. It must carry what is not recoverable from the code: why the AST is an index arena,
  why `ShIo` gained a capture pointer and that it works only because the buffer-once discipline
  was already there, why a here-doc is a file and not a pipe, why `test` is a builtin, why the
  empty quoted word needed a flag the mask could not carry, the six impossibilities, and that
  **the syscall ABI did not change**.
- **[CLAUDE.md](../../CLAUDE.md)** — the "Known gaps" bullet, which S1 has already begun, gains
  the rest of the residual gaps: no subshell isolation, no compound in the background, no `#!`,
  no `trap` that can ignore `^C`, no interrupting an all-builtin loop.
- **[Programming_Manual.md](../../doc/Programming_Manual.md)** — a short section on writing a
  script, since that becomes something an SDK user can do.
- New builtins get their one-line usage in `builtin/table.cpp`; `rootfs/share/help` only decorates
  `/bin`, so it needs nothing.

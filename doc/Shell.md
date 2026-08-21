# The Braam shell

`/bin/sh` is a Bourne shell, and the reference for every decision in it is v7's.
It has variables, functions, the three loops, `case`, globbing, here-documents,
command substitution, job control, twenty-six builtins and a line editor — and
it is an ordinary wasm binary in `/bin`, running in a worker of its own, asking
for everything it needs through the same system calls any program can call. This
document is the whole of it.

Concept.md §4.5 is the specification and this is derived from it: where the two
disagree about intent, §4.5 wins. [Programming_Manual.md](Programming_Manual.md)
§4 is the short form, for somebody writing a script from outside the tree; this
is the long form, for somebody using the shell. A bare `§N` below is always a
section of [Concept.md](Concept.md), never of this document.

---

## 1. Running the shell

```
usage: sh [-eux] [-s | -c <command> | <file>] [<arg>...]
```

Four ways in, and exactly one is picked out of the argument list:

| Form | Reads from | `$0` |
| --- | --- | --- |
| `sh` | the keyboard, at a prompt | argv[0] — `/bin/sh` for the shell init starts |
| `sh <file>` | the file, read whole | the file's path |
| `sh -c <command> [name [arg...]]` | the string | `name` if one follows, else argv[0] |
| `sh -s [arg...]` | standard input, a line at a time | argv[0] |

Operands after the file, after `-c`'s command word, or after `-s` become `$1`
onwards.

**Only the first form is interactive.** It alone claims the keyboard, hands the
foreground to what it runs, draws a prompt, and announces finished background
jobs. The other three do none of that, and the differences that follow from it
are collected in §13 and §12.

`-e`, `-u` and `-x` are the three options, and may be bundled (`-eux`); they are
exactly what `set` toggles later. **Options are not permuted**, so `sh file -x`
passes `-x` to the script as `$1`. `--` ends them, and so does the first word
that is not an option. An unknown letter, or `-c` with nothing after it, prints
the usage line on standard error and exits 2.

**A file is parsed whole before any of it runs**, as `.` is, so a syntax error
on the last line means the first line does not run either. That is the price of
the shell keeping its standard input free for the script to read from, which is
what lets `while read l; do …; done` inside a script work against a pipe.
`sh -s` is the exception: it reads a line at a time, so `producer | sh -s`
streams.

**`#!` works.** A file whose first line is `#!` followed by an **absolute** path
is executable, so `./script.sh` runs. The interpreter must be absolute — `PATH`
is the caller's, so a bare word would name a different interpreter depending on
who ran the script — the lookup is one level deep, so an interpreter that is
itself a script is refused, and the first line must end within 128 bytes. The
mechanism is the kernel's, not the shell's; §4.3 has it.

```
$ cat greet
#!/bin/sh
echo greetings $1
$ ./greet world
greetings world
$ sh -c 'echo $0 $1' name arg
name arg
```

---

## 2. The prompt and the line editor

**There is no `PS1`.** The prompt is three coloured runs rather than a string,
because a colour here is an argument to a system call and not an escape in the
bytes (§2.3):

- the previous command's status as `[N] ` in red, and nothing at all when it was
  zero;
- the **basename** of the working directory, white on blue;
- ` $ `, in bright white.

The directory is asked for afresh each time rather than remembered, since `cd`
is not the only thing that can move a shell. So a prompt reads `home $ `, and
`[1] home $ ` after a failure.

`PS2` is an ordinary variable, default `> `, and is the whole prompt — no
status, no directory — while a construct is unfinished. `PS4` is the `set -x`
prefix, default `+ `.

The editor is a line discipline in userland, with no termios and no escape
sequence anywhere in it. Every binding:

| Key | Effect |
| --- | --- |
| any printable key | insert at the cursor |
| `Enter` | commit the line, and remember it |
| `^C` | abandon the line; prints `^C`, status 130 |
| `Backspace`, `^H` | delete the character before the cursor |
| `Alt+Backspace` | delete the word before the cursor |
| `Delete`, `^D` | delete the character **under** the cursor |
| `Left`, `^B` / `Right`, `^F` | move by one character |
| `Home`, `^A` / `End`, `^E` | to the start / to the end |
| `^K` / `^U` | delete to the end / to the start |
| `^W` | delete the word before the cursor |
| `^L` | clear the screen and repaint |
| `Up` / `Down` | walk the history |

Anything else changes nothing. **`^D` does not end the shell** — it deletes
forward, and at the end of a line it does nothing; `exit` is how a session ends.
**There is no Tab completion**: Tab is unbound.

A word ends at a space or a tab and nowhere else, which is what `^W` and
`Alt+Backspace` walk back over. History holds **thirty-two** lines, oldest
dropped; an empty line is not remembered, nor is one identical to the line
before it. **History does not persist** — it is a vector in the shell process,
and a new shell starts with none. `Up` parks the line being typed and `Down`
past the newest entry brings it back.

Scrollback is the page's, not the shell's: `Shift+PageUp`/`Shift+PageDown` page,
`Shift+Up`/ `Shift+Down` and the mouse wheel move a row, and any other key
returns to the live screen.

---

## 3. Words, quoting and comments

A line is split into words and operators before anything else happens. Blanks
are the space, tab, `\r`, `\v` and `\f`; **a newline is not a blank** but a
separator in its own right, since one text may be many lines.

The operator characters are `| & < > ; ( )`. A word runs to the next unquoted
blank, newline or operator.

`#` begins a comment only where a token could start, so `a#b` is one word and
`a # b` is a word and a comment.

Three ways to quote, and **the lexer removes none of them** — that is
expansion's job, one step later, which is what lets a quoted `*` be told from a
live one:

- **`'…'`** — everything inside is literal. There are no escapes at all, so a
  single quote cannot appear inside single quotes.
- **`"…"`** — `$`, `` ` `` and `\` still act. Only `\"`, `\\` and `\$` are
  escapes; **every other backslash stands for itself**, so `"a\nb"` is `a`, a
  backslash, `n`, `b`.
- **`\`** outside quotes — the next byte is literal. A backslash at the very end
  of the text is an unterminated quote, not a line continuation.

**Reserved words are reserved by position only**, and any quoting at all defeats
them, because the check is against the word as written. So `echo done` prints
`done`, and `for do in a; do …` uses `do` as a variable name. The reserved words
are `{` `}` `!` `if` `then` `elif` `else` `fi` `while` `until` `do` `done` `for`
`in` `case` `esac`.

`{` and `}` are among them, which means they are **words and not operators**:
`{` must be followed by a blank and `}` must be preceded by a `;` or a newline.
`{ a }` is an error — the `}` was taken as an argument to `a`.

**Only `2` is a recognised descriptor prefix**, and only when it is the whole
prefix of the token. `2>x` redirects standard error; `a2>x` is the word `a2` and
then `>`; and `1>x` is the **word `1`** and then `>`, so `echo hi 1>f` writes
`hi 1` to `f`. §15.

```
$ echo 'a  b'
a  b
$ echo a\ \ b
a  b
$ echo "a\nb"
a\nb
$ echo done
done
```

---

## 4. Commands and control flow

The grammar, which is §4.5's:

```
line     := list
list     := and_or (sep and_or)* [sep]
sep      := ';' | '&' | newline
and_or   := pipeline (('&&' | '||') pipeline)*
pipeline := ['!'] (funcdef | compound | simple ('|' simple)*)
funcdef  := name '(' ')' newline* compound
compound := group | subshell | if | loop | for | case
group    := '{' list '}'
subshell := '(' list ')'
if       := 'if' list 'then' list ('elif' list 'then' list)* ['else' list] 'fi'
loop     := ('while' | 'until') list 'do' list 'done'
for      := 'for' name ['in' word*] sep 'do' list 'done'
case     := 'case' word 'in' arm* 'esac'
arm      := ['('] word ('|' word)* ')' list [';;']
simple   := assign* (word | redirect)+ | assign+ redirect*
assign   := name '=' word, and only ahead of the first ordinary word
```

**A simple command** is optional assignments, then words and redirections in any
order. The first word is the command; it resolves as **a function, then a
builtin, then a file on `PATH`**, and only the last of the three costs a
process. A word containing `/` is a path instead, resolved against the shell's
own working directory and never searched for. `command -v <word>` says which of
the three a word is.

**The search is the kernel's.** The shell hands `Sys::Spawn` the word as typed
and `exec_resolve` searches `PATH` out of the environment that spawn carries, so
`timeout ls` and `watch ls` search the same list the shell does. Components are
separated by `:` and an empty one is skipped rather than meaning the current
directory; a relative one is resolved against the working directory. A file on
`PATH` that is not a program does not shadow one further along, and a search
that found only such files reports `not executable` (126) rather than
`not found` (127). An environment naming no `PATH` at all searches `/bin`.

**A pipeline** joins simple commands with `|`, at most **eight** stages. Its
status is its last stage's. `!` may lead the whole pipeline and inverts that
status. Every stage runs at once, in a worker of its own — except a builtin or a
function, which runs **in its turn** rather than alongside, and therefore
buffers its output and writes it once.

**`&&` and `||`** chain pipelines left to right, each running only if the one
before it succeeded or failed. `;` and a newline separate; `&` puts the pipeline
in the background (§11).

**`{ list; }`** groups without isolating anything. **`( list )`** is a subshell:
there is no `fork` here, so it runs in this same process with the working
directory, the variables, the positional parameters, the function table, the
options and the `exec` base saved and put back around it. `(cd /x; ls)` and
`(set -e; …)` are exact; only memory isolation is lost, and the job table is
deliberately shared, since its children are still this process's to reap.

**`if`**, **`while`**, **`until`**, **`for`** and **`case`** are v7's. A `for`
with no `in` walks the positional parameters. A `case` pattern is matched by
§7's matcher, alternatives are separated by `|`, and an arm ends at `;;`.

**A function** is `name () compound`. It is looked up before any builtin and
before `/bin`, so it may shadow either. It runs in the shell's own turn on the
caller's state: `$1`…`$#` are saved and put back, but **its variables and its
working directory are the shell's** — a function is not a scope. It inherits the
stage's descriptors, so `f | wc` and `f > log` work. `return` leaves it;
`unset -f` removes it.

**Three statuses are not the last command's**: an `if` that took no branch, a
loop whose body never ran, and a `case` that matched no arm all report **0**,
where v7 leaves the last condition's status.

A text nests **sixteen** deep in the parser and sixty-four in the walk.

```
$ ls /bin | grep tai
tail
$ ! true; echo $?
1
$ (cd /bin; pwd); pwd
/bin
/home
$ f() { echo "$1 and $2"; }
$ f one two
one and two
```

---

## 5. Redirection

Eight operators, and no others:

| Operator | Effect |
| --- | --- |
| `< w` | standard input from `w` |
| `> w` | standard output to `w`, created and truncated |
| `>> w` | standard output to `w`, created and appended |
| `2> w` | standard error to `w`, created and truncated |
| `2>> w` | standard error to `w`, created and appended |
| `>& n` | standard output onto stream `n` |
| `2>& n` | standard error onto stream `n` |
| `<< w`, `<<- w` | standard input from a here-document |

The operand of `>&` and `2>&` must be literally `1` or `2`, checked when the
line is parsed; `>&-` is refused by name. **The copy is of the stream as it
stands at that point**, so `> f 2>&1` sends both to the file and `2>&1 > f`
sends only standard output there. A second redirection of the same stream
replaces the first, closing what the first opened. A redirection that cannot be
opened stops the command before it runs.

Redirections are applied after the pipes are made, so one displaces a pipe end.
They may lead the command — `> f ls` is fine — but a redirection with no command
word at all is an error.

A compound command takes its own: `{ …; } > f` and `for … done > f` redirect the
whole body.

**A here-document's body is the lines after the line the operator is on**, up to
a line equal to the delimiter, and it reaches the command as a pipe rather than
a file. `<<-` strips leading **tabs** — only tabs — from the body and from the
terminator. If the delimiter carried any quoting at all, the body is taken
literally; otherwise `$` expansion acts in it, while quotes inside it do not.

```
$ ls /nope 2>&1 | wc
1 4 21
$ v=world
$ cat <<EOF
> hello $v
> EOF
hello world
$ cat <<'EOF'
> hello $v
> EOF
hello $v
```

---

## 6. Word expansion

**Expansion is two passes and quote removal happens in the first.** Parameter
expansion, command substitution, field splitting and quote removal are one
left-to-right walk over the word; what comes out of it then goes through
file-name generation (§7).

Across a pipeline the order is: every assignment's value, then every command's
words, then every redirection target. **Assignment values and redirection
targets expand to exactly one field** — they never split and are never globbed,
which is why `> *.txt` writes to a file named `*.txt` rather than silently to
whatever it matched.

Within a word, left to right:

| Written | Is |
| --- | --- |
| `$name` | a variable; the name is `[A-Za-z_][A-Za-z0-9_]*` |
| `$0`…`$9` | a positional parameter — **one digit**, so `$10` is `$1` then `0` |
| `$#` `$?` `$$` `$!` `$-` `$*` `$@` | §8 |
| `${…}` | the same, and the four operators below |
| `$(…)`, `` `…` `` | command substitution |
| `\c`, `'…'`, `"…"` | quoting, removed here |

A `$` followed by anything else is an ordinary `$`.

**The four `${…}` operators**, and there are no others:

| Written | Means |
| --- | --- |
| `${x-word}` | `word` if `x` is **unset**, otherwise `x` |
| `${x=word}` | assign `word` to `x` if unset, then use it |
| `${x?word}` | fail with `word` if unset; the default message is `parameter not set` |
| `${x+word}` | `word` if `x` **is** set, otherwise nothing |

**There are no colon forms.** `${x-y}` asks whether `x` is *set*, empty or not;
`${x:-y}` is `bad substitution`. There is no `${#x}`, no `${x#pat}` or
`${x%pat}`, no `${x/…}`, and `${10}` does not name the tenth parameter. `${1=x}`
is `cannot assign to this parameter`. Expansion nests eight deep.

**Command substitution** runs the text and puts its output in the word, with
trailing newlines stripped. `$(…)` and `` `…` `` differ only in that a backslash
inside backticks is literal except before `` ` ``, `\` and `$`. **The output is
unquoted**, so a `*` that comes out of one still globs and still splits. A
`$( )` is **not** a subshell and takes no checkpoint: `$(cd /x)` really moves
the shell and `$(y=1)` really sets a variable.

**Field splitting** uses `$IFS` — default space, tab and newline, and only its
first sixteen bytes are read. It applies **only to bytes an unquoted expansion
produced** — a separator the word itself contains never splits, so with `IFS=:`
a typed `a:b` is one field and `$path` is two. A run of separators is one break,
and leading and trailing separators make no empty fields. An empty `IFS` splits
nothing.

`""` and `''` produce an empty field. `"$@"` with no positional parameters
produces **no field at all** — the one exception. `"$*"` joins with the first
byte of `IFS`.

Every byte of an expanded word carries a mark saying whether it came from
quotes. §7 and `case` are its only readers, and it is what makes `'a*'` a
literal star while `$star` is a live one.

**There is no tilde expansion, no arithmetic expansion and no brace expansion.**
`$((a+b))` is a command substitution whose body is `(a+b)`.

```
$ x=abc; echo ${x}s
abcs
$ echo ${nosuch-alt}
alt
$ echo $(pwd)/x
/home/x
$ IFS=:; p=a:b; echo $p
a b
```

---

## 7. Patterns and file names

One matcher serves both `case` and file-name generation:

- `*` — any run of bytes, including none.
- `?` — exactly one byte, never zero.
- `[…]` — one of the bytes named. `!` first is negation, and **only `!`** —
  `[^…]` is not. Ranges are `a-z`, compared as unsigned bytes. A `-` first or
  last is literal. There are no character classes and no backslash inside the
  group; quoting is expressed through §6's mark instead. **An unterminated `[`
  matches nothing**, which is v7's answer.

A quoted metacharacter matches itself and never triggers a directory listing.

**File-name generation runs on argv words and on a `for … in` list, and nowhere
else.** The pattern is walked component by component: a component with no live
metacharacter is taken on trust and costs no listing, and one with a
metacharacter lists what it is under.

- **A leading dot must be asked for**: `*` does not match `.profile`, `.*` does.
  It is a first-byte test, so `.[a-z]*` works too.
- **A trailing `/` matches directories only**, and so does every non-final
  component.
- **No match leaves the word exactly as written** — Bourne's rule, not an error.
- Results come out in the store's own order. **They are not sorted.**

```
$ echo /bin/l*
/bin/less /bin/ls
$ echo '/bin/l*'
/bin/l*
$ echo /home/g/*/
/home/g/sub/
$ case hi in h*) echo yes ;; esac
yes
```

---

## 8. Parameters and variables

An assignment is `name=value` ahead of the first ordinary word of a command.
`x=` is a valid empty assignment; `ls x=1` passes an argument; `=1` and `2a=1`
are ordinary words.

**A `x=1 cmd` prefix behaves three different ways**, and the difference is which
process owns the state:

- the command has **no words at all** (`x=1`) — the assignment is the shell's,
  permanently;
- the command is a **builtin or a function** — applied for its turn and then put
  back;
- the command is a **program** — it goes into that child's environment only,
  replacing an exported variable of the same name there, and the shell's own
  table is untouched.

The special parameters:

| | Is |
| --- | --- |
| `$0` | the shell's name, or the script's path |
| `$1`…`$9` | the positional parameters; `$#` does not count `$0` |
| `$#` | how many there are |
| `$*` `$@` | all of them; see §6 for what quoting does |
| `$?` | the last command's status |
| `$$` | this process's pid |
| `$!` | the last background job's pid |
| `$-` | the option letters currently on, in the order `e`, `u`, `x` |

`set -- a b c` replaces them, `set --` clears them, and `shift [n]` drops the
first `n`.

**The shell reads exactly five variables**: `IFS` (§6), `PS2` and `PS4` (§2),
`HOME`, which `cd` with no operand uses and which falls back to the literal
`/home`, and `PATH`, which `command -v` walks — though what *runs* a
command word is the kernel reading the same variable out of the environment
(§4). It plants none of its own. The environment init hands `/bin/sh` is
`PATH=/bin`, `HOME=/home` and `SHELL=/bin/sh`, and everything in an incoming
environment becomes an exported shell variable at startup, so a nested `sh` is
not a wall.

**`PATH` is exported whether or not it is marked.** Assigning it marks it, since
an unexported one would never reach the kernel that reads it. Unsetting it is
allowed and means the kernel's own default, `/bin`.

**Scoping is global.** A function call swaps only the positional parameters.
Only `( … )` isolates variables, and it does so by saving and restoring them,
not by forking.

`export` marks a variable to be copied into every child's environment at spawn;
`readonly` refuses further assignment. **Neither mark ever comes off again.** A
process's environment is fixed when it is spawned — there is no `setenv` and no
`Sys::Env` — so `export` reaches a child and nothing changes a running process's
own.

```
$ x=1; export x; sh -c 'echo $x'
1
$ set -- p q; echo $# $2
2 q
$ readonly r=keep; r=other
r: cannot be set
```

---

## 9. Conditions: `test` and `[`

`test <expr>` and `[ <expr> ]` are the same evaluator, and it is v7's grammar.
`[` requires the closing `]`, which is discarded.

Loosest to tightest: `-o`, then `-a`, then `!`, then a primary. `( expr )`
groups, and the parens must be separate words.

| Primary | True when |
| --- | --- |
| `-r f` | `f` exists — there are no file permissions here, so this is existence |
| `-w f` | `f` is on a writable mount and can be opened for writing |
| `-x f` | the kernel would instantiate `f`: it begins `\0asm`, or is a valid `#!` |
| `-f f` / `-d f` | `f` is a regular file / a directory |
| `-h f` / `-L f` | `f` is a symbolic link — two names for one primary |
| `-s f` | `f` is not empty |
| `-t [n]` | descriptor `n` is a console; with no operand, descriptor 1 |
| `-n s` / `-z s` | `s` is not empty / is empty |
| `s = s2`, `s != s2` | the strings compare |
| `n -eq -ne -gt -ge -lt -le n2` | the numbers compare |
| one bare word | it is not empty |

**Every primary above follows a symbolic link except `-h`/`-L`**, which is
what makes them the only two that answer for a link that dangles: `-f` on one
is false because there is nothing at the end of it, and `-h` is true because the
link is there.

**`-a` and `-o` do not short-circuit** — both sides are always evaluated,
because the file tests are answered in a pass of their own before the expression
is walked.

Numbers are v7's `atoi`: an optional sign, digits until the first byte that is
not one, and zero for a word with no digits at all. So `test x -eq 0` is
**true**.

The status is **0** true, **1** false, and **2** for an expression that is
neither — which prints `test: argument expected`, `test: ) expected`,
`test: unknown operator`, `test: too deeply nested`, or `test: ] missing`. An
empty expression is false, not an error.

Both are builtins **and** files in `/bin`, because their whole cost was the
spawn. Typing the name runs the builtin; `/bin/test` gives the same answers.

```
$ [ -f /etc/motd ] && echo yes
yes
$ test 12x -eq 12; echo $?
0
$ [ -f /bin
test: ] missing
```

---

## 10. The builtins

Twenty-six, and the list is closed. A builtin is one of two things: something
that touches the shell **process's** own state and so could not be a file — its
working directory, its job table, its variables, its options, its traps, its
loop — or something whose **whole cost is the spawn**, which is `test`, `[`,
`:`, `echo`, `true` and `false` and nothing else. Those six keep their file in
`/bin`, since a builtin shadows the name at a prompt and not everywhere; the
others have no file and never will.

A builtin is an ordinary pipeline stage and redirects like anything else, so
`jobs | grep sleep` works — but it runs **in its turn** rather than alongside,
so it buffers its output and writes it once.

Unless said otherwise, a usage error prints `usage: …` on standard error and
exits **2**.

### `. <file>`

Reads the file, parses it whole, and runs it in the walk already in progress, so
it can set variables and define functions. `return` works inside it. A missing
file exits 127, a syntax error 2. There is no `source` alias.

### `: [<arg>...]`

Does nothing, successfully. Its redirections and assignment prefix still act, so
`: > f` truncates a file.

### `[ <expr> ]`

§9. Without the closing `]`: `test: ] missing`, status 2.

### `break [<n>]` and `continue [<n>]`

Leave, or restart, the innermost loop — or `n` of them. `n` must be a positive
integer. **Outside a loop both are silent no-ops**, as in v7, and a leftover
request never crosses into the next line.

### `cd [<dir>]`

Changes the shell's working directory, which is what a command typed after it
inherits at spawn. With no operand, `$HOME`, or `/home` when that is unset or
empty. Failure prints `cd: <dir>: <why>` and exits 1. **There is no `cd -`, no
`$OLDPWD`, no `-P`/`-L` and no `CDPATH`**; a `-` operand is an ordinary
directory name. `pwd` is not a builtin — it is `/bin/pwd`, since only a system
call can say which process is asking.

### `command -v <name>...`

Says what each name would run, in the order a command word resolves: a function
or a builtin prints as the bare name, and anything else prints as the path
`PATH` found — the same probe `test -x` uses, so a file that is neither a wasm
module nor a `#!` script is not an answer. A name that is nothing prints nothing
and the builtin exits 1. Only `-v` is here; see §15.

```
$ command -v cd
cd
$ command -v ls
/bin/ls
```

### `echo [-n] [<word>...]`

Writes the words separated by single spaces, with a newline unless `-n` is the
first operand. **There are no escapes and no `-e`.**

### `eval [<arg>...]`

Joins the arguments with one space and runs the result in the current walk, so
it can set a variable or define a function. An empty text is 0; a syntax error
is 2.

### `exec [<command>]`

With no command, the stage's redirections become the shell's own and outlive the
line — which is how `exec > log` works. With a command, it spawns it, waits, and
**ends the shell** with its status, because there is no re-instantiate-in-place
here and a spawn makes a new pid.

A top-level `exec` redirection cannot be undone: there is no `/dev/tty` and no
way to name the stream the shell was handed. Inside `( … )` it is checkpointed
like everything else.

### `exit [<status>]`

Ends the shell with `status & 0xff`, default 0. It **asks rather than acts** — a
builtin runs in the middle of a line, and the request is read when the line is
finished — which is why `exit | cat` ends the shell at the next prompt and why
nothing else on `exit`'s own line runs. A non-numeric operand is
`exit: <arg>: invalid`, status 2.

### `export [<name>[=<value>]...]` and `readonly [<name>[=<value>]...]`

Mark a variable exported, or refuse further assignment to it. With no operands
each lists what it has marked, as `export NAME=value`. The value is assigned
before the mark, since the mark this very call makes would refuse it after. A
refusal is `export: <name>: cannot be set`, status 1. Neither mark ever comes
off.

An operand a `$` could not name — `export notes.txt` — is
`export: <name>: not a valid name` and status 1, checked before the value, so
such an operand sets nothing and marks nothing. The remaining operands are
still applied.

### `false` / `true`

Exit 1 / exit 0.

### `fg [%n]`

Brings a background job to the foreground: echoes its command text, gives it the
keyboard and the foreground, and waits. With no operand, the most recent job.
The `%` is optional. An unknown job is `fg: no such job`, status 1; a `^C` while
waiting is 130.

### `jobs`

Lists the background jobs, `[<id>]` then `+` for the current one or a space,
then `running` or `done`, then the text the pipeline was typed as. §11.

### `kill %n...`

Cancels every stage of a job — cancellation, which is all a kill can be for
something cooperative, backed by terminating the worker of a process that will
not stop. **Job ids only**: the `%` is optional, but there is no signal argument
and no bare pid, because `Sys::Kill` refuses anything that is not a child of the
caller. An unknown id is `kill: no such job` and status 1, and the remaining ids
are still tried.

### `read <name>...`

Reads one line from standard input and splits it across the names on `$IFS`.
**The last name takes the whole remainder**, separators included, trimmed at
both ends. A last line with no newline is still a line. End of input is status
1, which is what ends a `while read`; a `^C` is 130; a readonly name is
`read: <name>: permission denied` and status 1. A name a `$` could not name is
`read: <name>: not a valid name` and status 1, and every name is checked
**before** the read, so a usage error does not consume the line.

`Sys::Read` carries no length, so a chunk is whatever the writer wrote: what
followed the newline is kept in a pushback buffer keyed by the descriptor, and
dropped when the next read is on another one. **There is no `-r` and no `-p`.**

### `return [<status>]`

Leaves a function or a sourced file with `status & 0xff`. Outside either it is a
silent no-op.

### `set [-eux] [+eux] [--] [<arg>...]`

With no arguments, lists every variable as `NAME=value`. Otherwise `-` turns
options on and `+` turns them off (§12), and operands — after a `--`, or simply
following the options — replace the positional parameters, keeping `$0`.
`set --` with nothing after it clears them; **`set -x` alone leaves them
alone**, since `set -x` is not `set --`.

### `shift [<n>]`

Drops the first `n` positional parameters, default one. Too few to drop is
status 1, silently.

### `test <expr>`

§9.

### `trap [<action>|-] 0|2`

§12.

### `unset [-f] <name>...`

Removes variables, or functions with `-f` as the first operand. An absent name
is a success. A readonly variable is `unset: <name>: is read only` and status 1,
and the rest are still tried.

### `wait [%n...]`

Waits for the named jobs, or for all of them. Unlike v7's, it puts each job **in
the foreground** while it waits, because there are no signals here and being in
front is the only way a `^C` can reach anything. An unknown id is
`wait: no such job` on standard error and status 127, and the rest are still
waited for; a `^C` is 130.

---

## 11. Jobs

A pipeline followed by `&` is started, filed in the job table, and reported as
status 0. The shell announces it on standard error as the job id and the first
stage's pid, and `$!` becomes that pid. **Only a pipeline may go into the
background** — `a && b &`, `{ …; } &` and `while … done &` are refused — because
nothing inside a process can wait for a sibling task.

Ids count up from 1 and are never reused in a session. A background job's
standard input is at end of input from the start.

**A job has two states, running and done.** There is no stopped state, because
there is no `^Z` and no `bg`. The job table is the shell process's own memory,
so no syscall shows it to anybody and there is no `/proc/jobs`; the stages are
still ordinary tasks, so `/proc/<pid>` lists them, which is how the shell
notices a background job has finished.

Finished jobs are announced **before the next prompt** rather than wherever they
happened to end, so a notice never lands in the middle of a line being typed.
Liveness is read out of `/proc` rather than waited for, since the prompt has to
come back either way.

**`^C` reaches a running pipeline, and abandons a typed line.** The foreground
is a set of pids the shell arms before it waits; at a prompt it arms nothing, so
the interrupt arrives as an ordinary key. The shell releases the keyboard
**before** it spawns, so a full-screen program can claim it in its first step,
and takes it back after.

```
$ sleep -m 5000 &
[1] 42
$ jobs
[1]+ running sleep -m 5000
$ fg
sleep -m 5000
$ kill %1
```

---

## 12. Options and traps

Three options, and `sh` takes the same three letters on its command line:

- **`-e`** ends the shell on a non-zero status — except where a status is being
  *tested*, which is an `if` or `elif` condition, a `while` or `until`
  condition, every link of an `&&`/`||` chain but the last, and anything under
  `!`.
- **`-u`** makes an unset parameter an error rather than an empty word. `${x-y}`
  and `${x+y}` are exempt, since both have already asked.
- **`-x`** prints `$PS4` and then the fully expanded pipeline on standard error
  before anything is opened, with the assignment prefix included and stages
  joined by ` |`. It traces the simple commands inside a compound, not the
  compound.

The letters are the shell process's own state, so they are saved and put back
around `( … )`. `$-` reports them.

**`trap` has two signals**, because the system has none: `0` (or `EXIT`) and `2`
(or `INT`).

- `trap <action> <signal>...` sets one; `trap - <signal>...` removes one;
  `trap 0` and `trap 2` with no action are v7's reset form and remove it too.
- `trap` alone prints what is set, as `trap -- 'action' 0`.
- Any other number is `trap: <n>: unsupported`, status 1.
- **`trap '' 2` is refused** — `trap: cannot ignore an interrupt` — rather than
  accepted and quietly dropped, because the cancellation flag is sticky and once
  a `^C` has been delivered nothing can decline it.

The EXIT trap runs however the shell ends, and its own status is not the
shell's. The INT trap fires only in an **interactive** shell: in a script the
process itself is cancelled and every await after that answers `Err(Cancelled)`,
so a `trap … 2` there can never run. A trap's action is taken before it is run,
so it cannot fire itself.

The trace is written when every stage's words are known and before anything is
opened, which is after the `set` that turns it on has been decided but before it
has run — so `set -x` does not trace itself, and `set +x` does.

```
$ set -x
$ x=1 echo hi
+ x=1 echo hi
hi
$ set +x
+ set +x
$ trap 'echo bye' 0
$ trap
trap -- 'echo bye' 0
```

---

## 13. Exit status

| Status | Means |
| --- | --- |
| 0 | success |
| 1 | the command failed; also a failed expansion, or a redirection that would not open |
| 2 | a usage error, or a syntax error in a line, in `eval`, or in a sourced file |
| 126 | the file is there and will not run: not executable, built for another process ABI, or too many processes |
| 127 | not found — a command, a sourced file, or a script named to `sh` |
| 130 | interrupted |

**130 is this shell's SIGINT.** There are no signals and a status is the only
thing that crosses a process boundary, so a stage reporting 130 stops the rest
of the text — and a program that exits 130 of its own accord does the same,
which is the price of having no other channel.

The runtime's own diagnostics are `<what>` and `<what>: <why>`; a builtin's are
`<name>: <what>: <why>`. Nothing is prefixed with the system's name: everything
here is Braam.

A failed expansion ends a **non-interactive** shell rather than letting a script
run past a hole; at a prompt it abandons the line and asks again.

---

## 14. The programs in `/bin`

Everything not in §10 is an ordinary program in `/bin`, in a worker of its own —
`/bin` being where the archive puts them and what `PATH` names at boot. `help`
is a `#!` script that pages `/etc/help`, which carries this list and the
builtins with it; each program also answers for itself.

| | |
| --- | --- |
| `cat` | copy files, or the input, to the output |
| `chat` | talk over a WebSocket to whoever else is there |
| `clear` | blank the screen |
| `curl` | fetch a URL |
| `date` | print the date and time |
| `df` | report filesystem space |
| `echo` | write the arguments — also a builtin |
| `edit` | a full-screen editor; `^S` saves, `^Q` quits |
| `env` | print the environment, or run a command with it changed |
| `false` | fail — also a builtin |
| `fexport` | download a file out of the browser |
| `fimport` | copy files from the browser into `/import` |
| `grep` | pass matching lines; a substring search, with no regular expressions |
| `head` | the first lines, ten by default |
| `help` | page `/etc/help`, which is this list and the builtins |
| `hog` | take memory until the cap refuses more |
| `less` | page a file on a terminal, copy it off one; `q` quits |
| `ln` | make a symbolic link |
| `ls` | list directories, in columns on a terminal |
| `mkdir` | create directories |
| `mount` | list the mounted filesystems |
| `mv` | move or rename files |
| `pbcopy` / `pbpaste` | the system clipboard, in and out |
| `ps` | list the tasks the kernel is running |
| `pwd` | print the working directory |
| `rm` | remove files, or directories with `-r` |
| `sh` | the shell |
| `sleep` | wait |
| `spin` | loop without yielding, to be killed |
| `tail` | the last lines, ten by default |
| `test` | evaluate a condition — also a builtin |
| `timeout` | run a command, killing it after a delay |
| `touch` | create empty files |
| `true` | succeed — also a builtin |
| `uname` | name the system |
| `vmstat` | report what the kernel is doing, as rates |
| `watch` | run a command over and over |
| `wc` | count lines, words and bytes |

---

## 15. What is not here

Each of these is absent on purpose, and each has its reasoning in §4.5 or in
[Release_Notes.md](Release_Notes.md). None is a bug, and adding one is a design
change to argue in Concept.md first.

**The language:**

- **No tilde, arithmetic or brace expansion.** `~`, `$(( ))` and `{a,b}` are not
  expansions.
- **No colon forms and no editing forms in `${…}`.** `${x:-y}`, `${#x}`,
  `${x#pat}`, `${x%pat}` and `${x/…}` are all `bad substitution`; the four
  operators in §6 are the whole set.
- **`$10` is not the tenth parameter.** A positional parameter is one digit;
  `shift` reaches the rest.
- **Only descriptors 1 and 2 exist**, so `n> f` for any other `n` is a word,
  `<&` and `n>&m` are not operators, and `>&-` is refused. Closing a stream
  would need a value for it in the spawn payload and a null sink in the kernel,
  and there is neither.
- **A compound command cannot be piped.** `{ a; } | wc` is a syntax error,
  because a pipeline's stages are commands and making them nodes is its own
  change. Redirecting one works.
- **Only a pipeline may go into the background**, for the reason in §11.
- **`( … )` isolates state, not memory.** There is no `fork`, so a runaway
  inside a subshell still spends the shell's 16 MB.
- **`$( )` is not a subshell and takes no checkpoint.** `$(cd /x)` moves the
  shell; `$(exit)` does not end it. Its output is bounded only by the process's
  memory cap.
- **A function is not a scope.** Its variables and its working directory are the
  shell's, and a `break` inside one reaches a loop outside it. There is no
  `local`.
- **`(` and `)` are tokens with no grammar above a subshell and a `case` arm**,
  so `echo (x)` is a syntax error rather than a word.
- **A script is parsed whole**, so a syntax error anywhere means none of it
  runs, where v7 runs everything above the error.
- **`read` reads past the line it was asked for**, keeping the remainder in a
  pushback buffer. `sh -s` has a reader of its own, so a `read` inside a script
  off standard input sees a different position in the same stream.
- **No `command <cmd>`.** Only the query half, `command -v`, is here:
  suppressing function lookup for one command has to reach the per-stage
  resolution in `job.cpp`, and a builtin runs after that decision rather than
  before it. A bare `command` is a usage line and status 2.

**The environment:**

- **No `PS1`.** The prompt is structural (§2).
- **No startup file** — no `.profile`, no `.shrc`, no `ENV`, no `--login`.
- **No `setenv`.** A process's environment is fixed at spawn. `export` reaches a
  child, but nothing changes a running process's own.
- **`$$` is not unique per shell.** It is the process's pid, and a top-level
  `/bin/sh` reports init's, since it is a process inside init's task rather than
  a job of its own. Nothing here derives a file name from it.
- **No file permissions**, so `-r` is existence and `-w` is a writable mount.

**The session:**

- **No `bg` and no `^Z`.** Stopping a running coroutine at an arbitrary point is
  the resume-side twin of cancellation and would have to reach every awaitable.
- **No Tab completion**, and history does not persist.
- **`kill <pid>` is gone; `kill %n` is not.** `Sys::Kill` refuses anything that
  is not a child of the caller.
- **A loop whose body is entirely builtins cannot be interrupted.** The shell
  arms its *children* with the foreground and is never in its own foreground
  set, so a `^C` has nowhere to go — `while true; do echo …; done` is one of
  those, and `while true; do sleep 100; done` is not. The escape is killing the
  shell, which init then replaces.
- **A multi-line paste loses everything after the first command.** Pasting one
  line is exact.
- **No `printf`, `source`, `type`, `command`, `getopts`, `alias`, `hash`,
  `umask`, `times` or `ulimit`.**

**Costs, which are the point of several of the rules above:**

- Every command that is not a builtin or a function costs an instantiation and a
  worker, roughly a millisecond. A `#!` script costs one process, not two.
- Every system call a program makes is two `postMessage` hops, 34–45 µs, and
  bulk I/O pays it per 512-byte chunk.
- A keystroke at the prompt costs two round trips, and Enter to the next prompt
  costs five.

---

## 16. Where to read next

- [Concept.md](Concept.md) §4.5 — the specification this is derived from,
  including the table of what v7 has that this does not and what stands in its
  place. §4 is the process model and the rule that decides what may be a
  builtin.
- [Programming_Manual.md](Programming_Manual.md) §4 — writing and installing a
  script from outside the tree, and the `#!` rules in full.
- [System_Calls.md](System_Calls.md) — what a system call is, what a descriptor
  is, and how a pipe, a spawn and the terminal actually work underneath all of
  the above.
- [Release_Notes.md](Release_Notes.md) — why each of §15's absences is an
  absence.

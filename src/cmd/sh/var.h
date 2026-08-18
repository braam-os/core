// The shell's variables and positional parameters — this process's own state,
// which is what makes `set`, `shift`, `unset`, `export` and `readonly`
// builtins. There is no environment in the wasm ABI (Concept.md §4.3), so
// nothing here crosses a spawn: `export` records an intent and no more.
#pragma once

#include "expand.h"
#include "kernel/args.h"

struct VarEntry {
    String name, value;
    bool valued   = false; // `export x` names one without giving it a value
    bool exported = false;
    bool readonly = false;
};

// False is out of memory or a readonly variable.
bool var_set(Str name, Str value);

// False when the name has no value, which `${x-y}` and `${x?y}` must tell
// apart from a value that is empty.
bool var_get(Str name, Str &value);

// False for a readonly variable.
bool var_unset(Str name);

// Records a flag, making a valueless entry if there is none. Neither flag
// ever comes off again.
bool var_mark(Str name, bool exported, bool readonly);

usize var_count();
const VarEntry *var_at(usize i);

// Positional parameters. $0 is the shell's own name and $# does not count it.
bool args_set(Args a);
bool args_shift(usize n);
usize args_count();
Str args_at(usize i);

// Exchanges the whole block for `next` and returns what was there, which is
// what a function call does around its own. $0 is index 0 and rides with it.
Vec<String> args_swap(Vec<String> next);

// The variable table, the same way, for `( … )`. Restoring through var_set
// would trip the readonly refusal, and copying every entry costs a pair of
// Strings each; the caller hands back what it took.
Vec<VarEntry *> vars_swap(Vec<VarEntry *> next);

// A copy of the table deep enough to put back, and the release for one.
bool vars_copy(Vec<VarEntry *> &out);
void vars_drop(Vec<VarEntry *> &v);

// $$ and $0, planted once by the shell: nothing here makes a syscall.
bool var_init(u32 pid, Str name0);

void var_status(i32 s);    // $?
void var_last_bg(u32 pid); // $!

// The callbacks the expander is given, wired to everything above.
const Vars &shell_vars();

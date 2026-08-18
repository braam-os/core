// The shell's variables and positional parameters — this process's own state,
// which is what makes `set`, `shift`, `unset`, `export` and `readonly`
// builtins. An exported one crosses a spawn: vars_env is what a stage hands
// its child, and var_init takes this shell's own environment back in.
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

// The exported variables as NAME=value words, for a spawn. `store` owns the
// bytes and the words view it, so `store` must outlive them and must not be
// appended to afterwards. A name marked but never given a value is skipped.
bool vars_env(String &store, Vec<Str> &words);

// $$ and $0, and this process's environment into the table. Nothing here makes
// a syscall: the environment came in with _start.
bool var_init(u32 pid, Str name0);

void var_status(i32 s);    // $?
void var_last_bg(u32 pid); // $!

// The callbacks the expander is given, wired to everything above.
const Vars &shell_vars();

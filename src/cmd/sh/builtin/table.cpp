#include "decl.h"

namespace {

// Sorted, because `help` prints it in order and a linear search over
// twenty-seven entries is not worth an index.
constexpr Builtin TABLE[] = {
    { ".", "<file> — run a file's commands in this shell", builtin_dot },
    { ":", "[<arg>...] — do nothing, successfully", builtin_colon },
    { "[", "<expr> ] — evaluate a condition, as `test` does", builtin_bracket },
    { "break", "[<n>] — leave a loop, or n of them", builtin_break },
    { "cd", "[<dir>] — change the working directory, /home by default", builtin_cd },
    { "command", "-v <name>... — say what a command word would run", builtin_command },
    { "continue", "[<n>] — start a loop's next turn", builtin_continue },
    { "echo", "[-n] [<word>...] — write the arguments", builtin_echo },
    { "eval", "[<arg>...] — join the arguments and run them as a command", builtin_eval },
    { "exec", "[<command>] — keep its redirections, or run it and leave", builtin_exec },
    { "exit", "[<status>] — end the shell", builtin_exit },
    { "export", "[<name>[=<value>]...] — put a variable in every child's environment",
      builtin_export },
    { "false", "fail", builtin_false },
    { "fg", "[%n] — wait for a background job in the foreground", builtin_fg },
    { "help", "list the builtins and the programs in /bin", builtin_help },
    { "jobs", "list the jobs started with &", builtin_jobs },
    { "kill", "%n — cancel a job", builtin_kill },
    { "read", "<name>... — one line of input, split across the names", builtin_read },
    { "readonly", "[<name>[=<value>]...] — refuse further assignment to a variable",
      builtin_readonly },
    { "return", "[<status>] — leave a function or a sourced file", builtin_return },
    { "set", "[-eux] [--] [<arg>...] — the options, the parameters, or the variables",
      builtin_set },
    { "shift", "[<n>] — drop the first n positional parameters", builtin_shift },
    { "test", "<expr> — evaluate a condition and report it as a status", builtin_test },
    { "trap", "[<action>|-] 0|2 — run an action when the shell ends, or on ^C", builtin_trap },
    { "true", "succeed", builtin_true },
    { "unset", "[-f] <name>... — remove a variable, or a function with -f", builtin_unset },
    { "wait", "[%n...] — wait for a background job, or for all of them", builtin_wait },
};

} // namespace

const Builtin *builtin_find(Str name)
{
    for (const Builtin &b : TABLE)
        if (b.name == name)
            return &b;
    return nullptr;
}

Span<const Builtin> builtins()
{
    return Span<const Builtin>(TABLE, sizeof(TABLE) / sizeof(TABLE[0]));
}

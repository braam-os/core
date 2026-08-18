#include "decl.h"

namespace {

// Sorted, because `help` prints it in order and a linear search over eleven
// entries is not worth an index.
constexpr Builtin TABLE[] = {
    { "cd", "[<dir>] — change the working directory, /home by default", builtin_cd },
    { "exit", "[<status>] — end the shell", builtin_exit },
    { "export", "[<name>[=<value>]...] — mark a variable, which no child can see", builtin_export },
    { "fg", "[%n] — wait for a background job in the foreground", builtin_fg },
    { "help", "list the builtins and the programs in /bin", builtin_help },
    { "jobs", "list the jobs started with &", builtin_jobs },
    { "kill", "%n — cancel a job", builtin_kill },
    { "readonly", "[<name>[=<value>]...] — refuse further assignment to a variable",
      builtin_readonly },
    { "set", "[--] [<arg>...] — the positional parameters, or list the variables", builtin_set },
    { "shift", "[<n>] — drop the first n positional parameters", builtin_shift },
    { "unset", "<name>... — remove a variable", builtin_unset },
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

#include "decl.h"
#include "user/builtin.h"

namespace {

// Sorted, because `help` prints it in order and a linear search over six
// entries is not worth an index.
constexpr Builtin TABLE[] = {
    { "cd", "[<dir>] — change the working directory, /home by default", builtin_cd },
    { "exit", "[<status>] — end the shell", builtin_exit },
    { "fg", "[%n] — wait for a background job in the foreground", builtin_fg },
    { "help", "list the builtins and the programs in /bin", builtin_help },
    { "jobs", "list the jobs started with &", builtin_jobs },
    { "kill", "%n | pid... — cancel a job or a task", builtin_kill },
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

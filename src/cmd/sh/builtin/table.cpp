#include "decl.h"

namespace {

// Sorted, though nothing walks it in order any more: a linear search over
// twenty-six entries is not worth an index, and sorted is how it stays
// readable. What each one is for is /etc/help's, not a string here.
constexpr Builtin TABLE[] = {
    { ".", builtin_dot },
    { ":", builtin_colon },
    { "[", builtin_bracket },
    { "break", builtin_break },
    { "cd", builtin_cd },
    { "command", builtin_command },
    { "continue", builtin_continue },
    { "echo", builtin_echo },
    { "eval", builtin_eval },
    { "exec", builtin_exec },
    { "exit", builtin_exit },
    { "export", builtin_export },
    { "false", builtin_false },
    { "fg", builtin_fg },
    { "jobs", builtin_jobs },
    { "kill", builtin_kill },
    { "read", builtin_read },
    { "readonly", builtin_readonly },
    { "return", builtin_return },
    { "set", builtin_set },
    { "shift", builtin_shift },
    { "test", builtin_test },
    { "trap", builtin_trap },
    { "true", builtin_true },
    { "unset", builtin_unset },
    { "wait", builtin_wait },
};

} // namespace

const Builtin *builtin_find(Str name)
{
    for (const Builtin &b : TABLE)
        if (b.name == name)
            return &b;
    return nullptr;
}

#include "decl.h"

// Three commands that are only a status. Builtins by builtin.h's second
// clause: the spawn would be the whole of what they cost. Their arguments are
// ignored, but an assignment prefix and a redirection on one still happen —
// exec_pipeline applies both before a builtin runs, which is what makes
// `: > f` truncate and `x=1 :` set x for that turn.

Task<i32> builtin_colon(Args, ShIo)
{
    co_return 0;
}

Task<i32> builtin_true(Args, ShIo)
{
    co_return 0;
}

Task<i32> builtin_false(Args, ShIo)
{
    co_return 1;
}

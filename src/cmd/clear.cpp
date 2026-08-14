#include "proc/io.h"

// The terminal is a cell grid, not a byte stream (Concept.md §2.3), so there
// is no escape sequence to send: blanking it is an operation.
Task<i32> proc_main(Args)
{
    Result<SysReply> r = co_await sys_call(Sys::ScreenClear, 0);
    co_return r.is_err() ? 1 : 0;
}

#include "host.h"

// The kernel's half of host.h's declaration. src/proc/rt.cpp holds the other.
[[noreturn]] void panic_raw(const char *ptr, usize len)
{
    host_log(ptr, len);
    __builtin_trap();
}

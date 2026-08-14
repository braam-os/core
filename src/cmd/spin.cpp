#include "kernel/fmt.h"
#include "kernel/text.h"
#include "proc/io.h"

// A program that will not stop being asked to. It is the tier-3 demonstration
// (Concept.md §4.2): between syscalls the kernel has no hold on a process at
// all, so the only way out of the loop below is terminating the worker it runs
// in — which is what tier 3 buys and tier 2 cannot.
//
//     spin        loops until it is killed
//     spin N      N million turns and exits, which is how the tier's ordinary
//                 path is exercised without waiting for a kill
Task<i32> proc_main(Args args)
{
    // volatile because an infinite loop with no side effect is not required to
    // make progress, and clang is entitled to delete this one outright — the
    // demonstration would then quietly exit and prove nothing.
    volatile u32 sink = 0;

    u64 turns    = 0;
    bool forever = args.size() < 2;
    if (!forever) {
        Option<u32> n = parse_u32(args[1]);
        if (!n.has_value()) {
            co_await errln("spin", args[1], Error::Invalid);
            co_return 2;
        }
        turns = u64(n.value()) * 1000000;
    }

    // Two branches rather than a ternary over two literals: the ternary is a
    // const char * the compiler will not fold, and Str would reach for strlen.
    Buf<64> b;
    b.put("spin: pid ").put(proc_pid());
    if (forever)
        b.put(", spinning\n");
    else
        b.put(", spinning briefly\n");
    if ((co_await write_all(SYS_STDOUT, b.str())).is_err())
        co_return 1;

    for (u64 i = 0; forever || i < turns; i++)
        sink = sink + 1;

    co_return 0;
}

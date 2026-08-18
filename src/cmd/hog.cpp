#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "proc/io.h"

// Eats memory until the instance's ceiling refuses to move (Concept.md §4.1).
// A kernel applet cannot do this experiment: its heap is the kernel's, and the
// answer would be a dead system rather than a number.
Task<i32> proc_main(Args)
{
    // Whole spans, so the allocator asks memory.grow every time rather than
    // handing back a size class it already owns.
    constexpr usize CHUNK = 64 * 1024;
    usize taken           = 0;
    void *last            = nullptr;
    void *prev            = nullptr;
    while (void *p = heap_alloc(CHUNK)) {
        prev = last;
        last = p;
        taken += CHUNK;
    }

    // Two spans go back, not one: reporting the answer needs coroutine frames,
    // and the reply the kernel writes back is a size class of its own, which
    // needs a span of its own once nothing else is left.
    heap_free(last);
    heap_free(prev);
    taken -= 2 * CHUNK;

    u32 pages    = u32(__builtin_wasm_memory_size(0));
    bool refused = __builtin_wasm_memory_grow(0, 1) == usize(-1);

    Buf<128> b;
    b.put("hog: pid ").put(proc_pid());
    b.put(", took ").put(u32(taken >> 20)).put(" MiB");
    b.put(", memory is ").put(pages).put(" pages\n");
    b.put(refused ? "hog: memory.grow refused past the cap\n"
                  : "hog: memory.grow still succeeds — there is no cap\n");
    if ((co_await write_all(SYS_STDOUT, b.str())).is_err())
        co_return 1;
    co_return refused ? 0 : 1;
}

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
    while (void *p = heap_alloc(CHUNK)) {
        last = p;
        taken += CHUNK;
    }

    // The last span goes back, because reporting the answer needs coroutine
    // frames and there would otherwise be nowhere to put them.
    heap_free(last);
    taken -= CHUNK;

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

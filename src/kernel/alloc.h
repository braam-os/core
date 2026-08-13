// The kernel heap. Coroutine frames are the primary workload (Concept.md §8.2).
#pragma once

#include "traits.h"
#include "types.h"

struct HeapStats {
    usize spans;          // 64 KiB spans claimed from linear memory
    usize bytes_reserved; // spans * SPAN_SIZE
    usize bytes_in_use;   // sum of the size classes of live blocks
    usize allocs;
    usize frees;
};

// `base` is the first byte the heap may use; 0 means the linker's __heap_base.
void heap_init(u32 base);

void *heap_alloc(usize n);
void heap_free(void *p);

HeapStats heap_stats();

// The span-aligned address the heap actually starts at.
usize heap_origin();

// Size class a request of n bytes lands in, or n rounded up to whole spans.
usize heap_block_size(usize n);

// Typed allocation. Plain `new` is unusable here: operator new returns null on
// failure and -fno-exceptions means the expression would then construct at
// address zero. These check first, and return null instead.
template <class T, class... A>
T *heap_new(A &&...a)
{
    void *p = heap_alloc(sizeof(T));
    return p ? new (p) T(forward<A>(a)...) : nullptr;
}

template <class T>
void heap_delete(T *p)
{
    if (p) {
        p->~T();
        heap_free(p);
    }
}

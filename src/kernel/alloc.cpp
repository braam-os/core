#include "alloc.h"

#include "host.h"

extern "C" u8 __heap_base; // supplied by wasm-ld

// Linear memory is carved into 64 KiB spans, each serving one size class.
// A side table maps span index to class, so free() needs no per-block header:
// the class is `span_class[ptr >> 16]`. That keeps 16-byte alignment, costs no
// bytes per allocation, and makes both sized and unsized free O(1).

namespace {

constexpr usize SPAN_SHIFT = 16;
constexpr usize SPAN_SIZE  = usize(1) << SPAN_SHIFT;
constexpr usize PAGE_SIZE  = 65536;
constexpr usize MAX_SPANS  = 4096; // 256 MiB of addressable heap

constexpr u16 SIZE_CLASS[]  = { 16, 32, 48, 64, 96, 128, 192, 256, 384, 512 };
constexpr usize NUM_CLASSES = sizeof(SIZE_CLASS) / sizeof(SIZE_CLASS[0]);
constexpr usize MAX_SMALL   = 512;

constexpr u8 SPAN_UNUSED     = 0xFF;
constexpr u8 SPAN_FREE       = 0xFE; // head of a free run
constexpr u8 SPAN_FREE_CONT  = 0xFD; // interior of a free run
constexpr u8 SPAN_LARGE      = 0xFC; // head of a live multi-span block
constexpr u8 SPAN_LARGE_CONT = 0xFB;

constexpr u32 NO_SPAN = 0xFFFFFFFF;

// Header of a free run, written into the run's first bytes.
struct FreeRun {
    u32 next;  // span index of the next free run, address-ordered
    u32 count; // spans in this run
};

struct Heap {
    u8 span_class[MAX_SPANS];
    u32 span_run[MAX_SPANS]; // span count, valid on a SPAN_LARGE head

    void *free_list[NUM_CLASSES];
    u8 *bump[NUM_CLASSES];
    u8 *bump_end[NUM_CLASSES];

    u32 free_head;  // first free run, or NO_SPAN
    u32 next_span;  // lowest span never yet claimed
    u32 span_limit; // one past the highest usable span
    u32 first_span; // where the heap begins

    bool ready;
    HeapStats stats;
};

Heap h;

u32 span_of(const void *p)
{
    return u32(reinterpret_cast<usize>(p) >> SPAN_SHIFT);
}

u8 *span_addr(u32 i)
{
    return reinterpret_cast<u8 *>(usize(i) << SPAN_SHIFT);
}

usize class_of(usize n)
{
    for (usize c = 0; c < NUM_CLASSES; c++)
        if (n <= SIZE_CLASS[c])
            return c;
    return NUM_CLASSES;
}

// Extends linear memory so that `spans` spans starting at h.next_span exist.
bool grow_to(u32 spans)
{
    usize want_end = (usize(h.next_span) + spans) << SPAN_SHIFT;
    usize have_end = usize(__builtin_wasm_memory_size(0)) * PAGE_SIZE;
    if (want_end <= have_end)
        return true;
    usize pages = (want_end - have_end + PAGE_SIZE - 1) / PAGE_SIZE;
    if (__builtin_wasm_memory_grow(0, pages) == usize(-1))
        return false;
    return true;
}

FreeRun *run_at(u32 s)
{
    return reinterpret_cast<FreeRun *>(span_addr(s));
}

// Address-ordered insertion with coalescing, so adjacent frees rejoin.
void free_run_insert(u32 start, u32 count)
{
    u32 prev = NO_SPAN;
    u32 cur  = h.free_head;
    while (cur != NO_SPAN && cur < start) {
        prev = cur;
        cur  = run_at(cur)->next;
    }

    for (u32 i = 0; i < count; i++)
        h.span_class[start + i] = i == 0 ? SPAN_FREE : SPAN_FREE_CONT;

    FreeRun *self = run_at(start);
    self->next    = cur;
    self->count   = count;
    if (prev == NO_SPAN)
        h.free_head = start;
    else
        run_at(prev)->next = start;

    if (cur != NO_SPAN && start + count == cur) {
        FreeRun *next = run_at(cur);
        self->count += next->count;
        self->next        = next->next;
        h.span_class[cur] = SPAN_FREE_CONT;
    }

    if (prev != NO_SPAN) {
        FreeRun *p = run_at(prev);
        if (prev + p->count == start) {
            p->count += self->count;
            p->next             = self->next;
            h.span_class[start] = SPAN_FREE_CONT;
        }
    }
}

// First fit over the free runs, then fresh spans.
u32 span_run_take(u32 count)
{
    u32 *link = &h.free_head;
    u32 cur   = h.free_head;
    while (cur != NO_SPAN) {
        FreeRun *r = run_at(cur);
        if (r->count >= count) {
            u32 next  = r->next;
            u32 extra = r->count - count;
            if (extra > 0) {
                u32 rest           = cur + count;
                FreeRun *tail      = run_at(rest);
                tail->next         = next;
                tail->count        = extra;
                h.span_class[rest] = SPAN_FREE;
                next               = rest;
            }
            *link = next;
            return cur;
        }
        link = &r->next;
        cur  = r->next;
    }

    if (usize(h.next_span) + count > h.span_limit)
        return NO_SPAN;
    if (!grow_to(count))
        return NO_SPAN;

    u32 start = h.next_span;
    h.next_span += count;
    h.stats.spans += count;
    h.stats.bytes_reserved += usize(count) << SPAN_SHIFT;
    return start;
}

void *alloc_small(usize c)
{
    if (void *p = h.free_list[c]) {
        h.free_list[c] = *reinterpret_cast<void **>(p);
        return p;
    }

    usize size = SIZE_CLASS[c];
    if (h.bump[c] + size > h.bump_end[c]) {
        u32 s = span_run_take(1);
        if (s == NO_SPAN)
            return nullptr;
        h.span_class[s] = u8(c);
        h.bump[c]       = span_addr(s);
        h.bump_end[c]   = span_addr(s) + SPAN_SIZE;
    }

    u8 *p = h.bump[c];
    h.bump[c] += size;
    return p;
}
} // namespace

void heap_init(u32 base)
{
    usize start = base ? base : reinterpret_cast<usize>(&__heap_base);
    start       = (start + SPAN_SIZE - 1) & ~(SPAN_SIZE - 1);

    for (usize i = 0; i < MAX_SPANS; i++)
        h.span_class[i] = SPAN_UNUSED;
    for (usize c = 0; c < NUM_CLASSES; c++) {
        h.free_list[c] = nullptr;
        h.bump[c]      = nullptr;
        h.bump_end[c]  = nullptr;
    }

    h.free_head  = NO_SPAN;
    h.next_span  = u32(start >> SPAN_SHIFT);
    h.first_span = h.next_span;
    h.span_limit = MAX_SPANS;
    h.stats      = HeapStats{};
    h.ready      = true;

    if (h.next_span >= MAX_SPANS)
        panic("heap_init: base above the span table");
}

void *heap_alloc(usize n)
{
    if (!h.ready)
        panic("heap_alloc before heap_init");
    if (n == 0)
        n = 1;

    void *p;
    usize accounted;
    if (n <= MAX_SMALL) {
        usize c   = class_of(n);
        p         = alloc_small(c);
        accounted = SIZE_CLASS[c];
    } else {
        u32 count = u32((n + SPAN_SIZE - 1) >> SPAN_SHIFT);
        u32 s     = span_run_take(count);
        if (s == NO_SPAN)
            return nullptr;
        for (u32 i = 0; i < count; i++)
            h.span_class[s + i] = i == 0 ? SPAN_LARGE : SPAN_LARGE_CONT;
        h.span_run[s] = count;
        p             = span_addr(s);
        accounted     = usize(count) << SPAN_SHIFT;
    }

    if (!p)
        return nullptr;
    h.stats.allocs++;
    h.stats.bytes_in_use += accounted;
    return p;
}

void heap_free(void *p)
{
    if (!p)
        return;

    u32 s = span_of(p);
    if (s >= MAX_SPANS)
        panic("heap_free: pointer outside the heap");

    u8 c = h.span_class[s];
    if (c < NUM_CLASSES) {
        *reinterpret_cast<void **>(p) = h.free_list[c];
        h.free_list[c]                = p;
        h.stats.bytes_in_use -= SIZE_CLASS[c];
    } else if (c == SPAN_LARGE) {
        u32 count = h.span_run[s];
        h.stats.bytes_in_use -= usize(count) << SPAN_SHIFT;
        free_run_insert(s, count);
    } else {
        panic("heap_free: not an allocation");
    }
    h.stats.frees++;
}

HeapStats heap_stats()
{
    return h.stats;
}

usize heap_origin()
{
    return usize(h.first_span) << SPAN_SHIFT;
}

usize heap_block_size(usize n)
{
    if (n == 0)
        n = 1;
    if (n <= MAX_SMALL)
        return SIZE_CLASS[class_of(n)];
    return ((n + SPAN_SIZE - 1) >> SPAN_SHIFT) << SPAN_SHIFT;
}

// Coroutine frames allocate through these (Concept.md §8.2). With
// -fno-exceptions a failed allocation returns null; callers must check.
void *operator new(usize n)
{
    return heap_alloc(n);
}

void *operator new[](usize n)
{
    return heap_alloc(n);
}

void operator delete(void *p) noexcept
{
    heap_free(p);
}

void operator delete[](void *p) noexcept
{
    heap_free(p);
}

void operator delete(void *p, usize) noexcept
{
    heap_free(p);
}

void operator delete[](void *p, usize) noexcept
{
    heap_free(p);
}

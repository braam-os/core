#include "jsref.h"

#include "alloc.h"
#include "vec.h"

// The table itself. A zero-length array of externref is how clang spells a
// WebAssembly table, and it insists on the `static` keyword rather than an
// anonymous namespace. The builtins below are the only way to touch it.
static __externref_t g_table[0];

namespace {

// Grown in chunks, since table.grow is a host call and slots are one word.
constexpr u32 CHUNK = 8;

u32 g_size   = 0; // slots the table has
u32 g_next   = 1; // the first never-used slot; 0 is reserved for null
usize g_live = 0;

// A namespace-scope global must be trivially destructible, so the free list is
// built on first use and never torn down.
Vec<JsSlot> *g_free = nullptr;

bool grow()
{
    if (__builtin_wasm_table_grow(g_table, __builtin_wasm_ref_null_extern(), CHUNK) < 0)
        return false;
    g_size += CHUNK;
    return true;
}

} // namespace

JsSlot jsref_reserve()
{
    if (g_free && !g_free->empty()) {
        JsSlot s = g_free->back();
        g_free->pop();
        g_live++;
        return s;
    }
    if (g_next >= g_size && !grow())
        return 0;
    g_live++;
    return g_next++;
}

void jsref_release(JsSlot slot)
{
    if (slot == 0)
        return;

    __builtin_wasm_table_set(g_table, slot, __builtin_wasm_ref_null_extern());
    g_live--;

    if (!g_free)
        g_free = heap_new<Vec<JsSlot>>();

    // A slot the free list cannot record is simply never reused. Losing an
    // index is cheaper than dropping the object it held.
    if (g_free)
        g_free->push(slot);
}

__externref_t jsref_get(JsSlot slot)
{
    if (slot == 0 || slot >= g_size)
        return __builtin_wasm_ref_null_extern();
    return __builtin_wasm_table_get(g_table, slot);
}

void jsref_set(JsSlot slot, __externref_t obj)
{
    if (slot != 0 && slot < g_size)
        __builtin_wasm_table_set(g_table, slot, obj);
}

usize jsref_live()
{
    return g_live;
}

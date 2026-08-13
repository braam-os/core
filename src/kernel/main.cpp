// The kernel's exported surface. M1 added the scheduler's tick() and wake();
// M2 adds key() and resize(), completing the five exports of Concept.md §3.4.
// M3 boots the userland shell, which is now the only task init() spawns.

#include "alloc.h"
#include "fmt.h"
#include "host.h"
#include "hostcall.h"
#include "jsref.h"
#include "key.h"
#include "sched.h"
#include "screen.h"
#include "str.h"
#include "user/shell.h"
#include "version.h"

// Runs the static constructors, which is what builds the program registry
// (Concept.md §3.6). --no-entry leaves it uncalled, so the kernel calls it
// itself. The symbol is hidden, so this adds no export.
extern "C" void __wasm_call_ctors();

BRAAM_EXPORT("init") void init(u32 heap_base)
{
    f64 started = host_now();

    // The constructors run after heap_init, so one added later may allocate.
    heap_init(heap_base);
    __wasm_call_ctors();

    // A grid exists from the first instruction, so the kernel is never in a
    // screenless state and the banner below has somewhere to go. The host
    // reflows it to the measured geometry with its first resize().
    if (!screen_resize(80, 24))
        panic("braam: no screen");

    HeapStats s = heap_stats();
    Buf<128> line;
    line.put("braam ")
        .put(BRAAM_VERSION)
        .put(" — heap at ")
        .put_hex(u32(heap_origin()))
        .put(", ")
        .put(u32(s.bytes_reserved >> 10))
        .put(" KiB reserved, ")
        .put(u32(s.allocs))
        .put(" allocs, up in ")
        .put(u32((host_now() - started) * 1000))
        .put(" us");
    log(line.str());
    screen_write(line.str());
    screen_newline();

    if (!sched_spawn(shell()))
        panic("braam: the shell would not spawn");
}

// Drains the ready queue and reports the delay until the next timer, or -1
// when nothing is pending (Concept.md §3.4). Whatever the tick drew on the
// screen goes out as one damage rectangle at the end of it.
BRAAM_EXPORT("tick") i32 tick(f64 now_ms)
{
    i32 delay = sched_tick(now_ms);
    screen_flush();
    return delay;
}

// A token nothing waits on is normally a late event and nothing more. A host
// request is the exception: its awaiter may be gone while the host still holds
// the record's address, so the reply is what finally frees it.
BRAAM_EXPORT("wake") void wake(u32 token, u32 payload_ptr, u32 payload_len)
{
    if (!sched_wake(token, payload_ptr, payload_len))
        host_orphan_reply(token);
}

// How a JS object enters the kernel (Concept.md §3.7). The slot was reserved
// before the request that produces the object was issued, so this only stores;
// nothing is scheduled and no tick is re-entered.
BRAAM_EXPORT("ref") void ref(u32 slot, __externref_t obj)
{
    jsref_set(slot, obj);
}

// The fast path: it only queues, so it can never re-enter the scheduler, and a
// full queue drops the event rather than blocking the host.
BRAAM_EXPORT("key") void key(u32 code, u32 mods)
{
    keys().try_send(Key{ code, mods });
}

// Reflows the screen and returns the address of its descriptor, or 0 if the
// grid could not be allocated. This is where the renderer learns the geometry
// and re-derives its view, since it is the only call that moves the cells.
BRAAM_EXPORT("resize") u32 resize(u32 cols, u32 rows)
{
    return screen_resize(cols, rows);
}

// The kernel's exported surface. M1 adds the scheduler's tick() and wake();
// key() and resize() arrive with M2.

#include "alloc.h"
#include "fmt.h"
#include "host.h"
#include "sched.h"
#include "str.h"
#include "task.h"

namespace {

constexpr Str VERSION = "0.1.0-m1";

// Two of these interleave their sleeps, so a bare page shows the scheduler
// running without a shell to drive it. The shell replaces them in M3.
Task<i32> demo(u32 first_ms, Str first, u32 second_ms, Str second) {
    if ((co_await sleep_ms(first_ms)).is_err())
        co_return 1;
    log(first);
    if ((co_await sleep_ms(second_ms)).is_err())
        co_return 1;
    log(second);
    co_return 0;
}

} // namespace

BRAAM_EXPORT("init") void init(u32 heap_base) {
    f64 started = host_now();

    heap_init(heap_base);

    if (!sched_spawn(demo(10, "demo a1", 20, "demo a2")) ||
        !sched_spawn(demo(15, "demo b1", 10, "demo b2")))
        panic("braam: the demo tasks would not spawn");

    HeapStats s = heap_stats();
    Buf<160> line;
    line.put("braam ")
        .put(VERSION)
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
}

// Drains the ready queue and reports the delay until the next timer, or -1
// when nothing is pending (Concept.md §3.4).
BRAAM_EXPORT("tick") i32 tick(f64 now_ms) {
    return sched_tick(now_ms);
}

BRAAM_EXPORT("wake") void wake(u32 token, u32 payload_ptr, u32 payload_len) {
    sched_wake(token, payload_ptr, payload_len);
}

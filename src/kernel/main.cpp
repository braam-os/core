// The kernel's exported surface. M1 added the scheduler's tick() and wake();
// M2 adds key() and resize(), completing the five exports of Concept.md §3.4.

#include "alloc.h"
#include "fmt.h"
#include "host.h"
#include "key.h"
#include "sched.h"
#include "screen.h"
#include "str.h"
#include "task.h"

namespace {

constexpr Str VERSION = "0.1.0-m2";

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

// Echoes the keyboard onto the screen. The line discipline proper — history,
// editing, completion — is userland's, and arrives with M3 (Concept.md §3.5).
Task<i32> console() {
    screen_cursor(true);
    for (;;) {
        Result<Key> r = co_await keys().recv();
        if (r.is_err())
            co_return 1;

        Key k = r.value();
        if (k.printable())
            screen_put(k.code);
        else if (k.code == KEY_ENTER)
            screen_newline();
        else if (k.code == KEY_BACKSPACE)
            screen_backspace();
        else if (k.code == 'l' && (k.mods & MOD_CTRL))
            screen_clear();
    }
}

} // namespace

BRAAM_EXPORT("init") void init(u32 heap_base) {
    f64 started = host_now();

    heap_init(heap_base);

    // A grid exists from the first instruction, so the kernel is never in a
    // screenless state and the banner below has somewhere to go. The host
    // reflows it to the measured geometry with its first resize().
    if (!screen_resize(80, 24))
        panic("braam: no screen");

    if (!sched_spawn(demo(10, "demo a1", 20, "demo a2")) ||
        !sched_spawn(demo(15, "demo b1", 10, "demo b2")) || !sched_spawn(console()))
        panic("braam: the boot tasks would not spawn");

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
    screen_write(line.str());
    screen_newline();
}

// Drains the ready queue and reports the delay until the next timer, or -1
// when nothing is pending (Concept.md §3.4). Whatever the tick drew on the
// screen goes out as one damage rectangle at the end of it.
BRAAM_EXPORT("tick") i32 tick(f64 now_ms) {
    i32 delay = sched_tick(now_ms);
    screen_flush();
    return delay;
}

BRAAM_EXPORT("wake") void wake(u32 token, u32 payload_ptr, u32 payload_len) {
    sched_wake(token, payload_ptr, payload_len);
}

// The fast path: it only queues, so it can never re-enter the scheduler, and a
// full queue drops the event rather than blocking the host.
BRAAM_EXPORT("key") void key(u32 code, u32 mods) {
    keys().try_send(Key{code, mods});
}

// Reflows the screen and returns the address of its descriptor, or 0 if the
// grid could not be allocated. This is where the renderer learns the geometry
// and re-derives its view, since it is the only call that moves the cells.
BRAAM_EXPORT("resize") u32 resize(u32 cols, u32 rows) {
    return screen_resize(cols, rows);
}

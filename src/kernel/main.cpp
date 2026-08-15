// The kernel's exported surface. M1 added the scheduler's tick() and wake();
// M2 adds key() and resize(), completing the five exports of Concept.md §3.4.
// M3 boots the userland shell; init() spawns it and the console pump. M8
// adds sys() and sys_async(), which are a process's two imports arriving here
// with the pid the host bound to them (§4.3).

#include "alloc.h"
#include "fmt.h"
#include "host.h"
#include "hostcall.h"
#include "jsref.h"
#include "key.h"
#include "sched.h"
#include "screen.h"
#include "str.h"
#include "traits.h"
#include "user/boot.h"
#include "user/console.h"
#include "user/exec.h"
#include "version.h"

// Runs the static constructors. --no-entry leaves it uncalled, so the kernel
// calls it itself. The symbol is hidden, so this adds no export.
extern "C" void __wasm_call_ctors();

namespace {

// The shell's pid, which is init's: /bin/sh is a process inside init's task
// rather than a job of its own. Written once sched_spawn has said what it is
// and read a tick later, when the body first runs — a Task is lazy, so nothing
// has looked yet. Zero is what sched_spawn returns when it fails and what the
// terminal claims mean by "nobody", so a process must not answer to it.
u32 g_init_pid = 0;

} // namespace

BRAAM_EXPORT("init") void init(u32 heap_base)
{
    f64 started = host_now();

    // The constructors run after heap_init, so one added later may allocate.
    heap_init(heap_base);
    __wasm_call_ctors();

    // A grid exists from the first instruction, so the kernel is never in a
    // screenless state and the banner below has somewhere to go. The host's
    // first resize() moves the rows to the measured geometry but does not
    // re-wrap them (§3.5), so what wraps here stays wrapped: the banner has to
    // fit in 80 columns, which is what keeps it terse.
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
        .put(" KiB, ")
        .put(u32(s.allocs))
        .put(" allocs, up in ")
        .put(u32((host_now() - started) * 1000))
        .put(" us");
    log(line.str());
    screen_write(line.str());
    screen_newline();

    // The pump before the shell, and for good: it is the only receiver on the
    // keyboard channel (console.h), and the prompt claims a route through it
    // rather than reading keys itself.
    if (!sched_spawn(console_pump(), "tty"))
        panic("braam: the console would not start");

    // The mounts, and then /bin/sh — a program like any other, with no shell
    // left in here to be the exception (Concept.md §4).
    Task<i32> t = init_task(g_init_pid);
    g_init_pid  = sched_spawn(move(t), "init");
    if (!g_init_pid)
        panic("braam: init would not spawn");
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
// full queue drops the event rather than blocking the host. Returns 1 if the
// keystroke was queued and 0 if it was not, which is what lets the host feed a
// paste at the rate the console drains it (Concept.md §3.5).
BRAAM_EXPORT("key") u32 key(u32 code, u32 mods)
{
    return keys().try_send(Key{ code, mods }) ? 1 : 0;
}

// Reflows the screen and returns the address of its descriptor, or 0 if the
// grid could not be allocated. This is where the renderer learns the geometry
// and re-derives its view, since it is the only call that moves the cells.
BRAAM_EXPORT("resize") u32 resize(u32 cols, u32 rows)
{
    return screen_resize(cols, rows);
}

// The two syscall entries (Concept.md §4.3). An isolated process imports `sys`
// and `sys_async` from the host, which supplies closures bound to that
// process's pid and forwards to these — so the pid is the host's word, not the
// caller's, and process 7 has no way to speak as process 3.
//
// Both are entered from JS at top level, never from inside a kernel import,
// which is what keeps them as ordinary as key() or wake().
BRAAM_EXPORT("sys") i32 sys(u32 pid, u32 op, u32 a0, u32 a1, u32 a2)
{
    return exec_sys(pid, op, a0, a1, a2);
}

BRAAM_EXPORT("sys_async") i32 sys_async(u32 pid, u32 op, u32 token, u32 len)
{
    return exec_sys_async(pid, op, token, len);
}

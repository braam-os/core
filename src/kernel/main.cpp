// The kernel's exported surface. M0 has only init(); the scheduler exports
// (wake, tick, key, resize) arrive with M1 and M2.

#include "alloc.h"
#include "coroutine.h"
#include "fmt.h"
#include "host.h"
#include "result.h"
#include "str.h"
#include "vec.h"

namespace {

constexpr Str VERSION = "0.1.0-m0";

// Proves the coroutine shim links and runs: suspends once with a live frame
// across the suspend point, then resumes to completion.
struct Boot {
    struct promise_type {
        Boot get_return_object() {
            return Boot{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        std::suspend_always final_suspend() noexcept { return {}; }

        void return_void() {}

        void unhandled_exception() {}
    };

    std::coroutine_handle<promise_type> h;
};

Boot coroutine_check(int &out) {
    int local = 21;
    co_await std::suspend_always{};
    out = local * 2;
}

bool coroutine_ok() {
    int out = 0;
    Boot b = coroutine_check(out);
    if (!b.h)
        return false;
    b.h.resume(); // past initial_suspend, to the co_await
    b.h.resume(); // through the body, to final_suspend
    bool done = b.h.done();
    b.h.destroy();
    return done && out == 42;
}

} // namespace

BRAAM_EXPORT("init") void init(u32 heap_base) {
    f64 started = host_now();

    heap_init(heap_base);

    if (!coroutine_ok())
        panic("braam: coroutine shim is broken");

    Vec<u32> v;
    for (u32 i = 0; i < 64; i++)
        v.push(i * i);
    if (v.size() != 64 || v[63] != 63 * 63)
        panic("braam: Vec self-check failed");

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

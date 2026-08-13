#include "harness.h"
#include "kernel/alloc.h"
#include "kernel/coroutine.h"

namespace {

int live_guards;

struct Guard {
    Guard() { live_guards++; }

    ~Guard() { live_guards--; }
};

struct Simple {
    struct promise_type {
        u32 value = 0;

        Simple get_return_object()
        {
            return Simple{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        std::suspend_always final_suspend() noexcept { return {}; }

        void return_value(u32 v) { value = v; }

        void unhandled_exception() {}
    };

    std::coroutine_handle<promise_type> h;
};

Simple add(u32 a, u32 b)
{
    u32 carried = a;
    co_await std::suspend_always{};
    co_return carried + b;
}

// Holds an object across the suspend point, so destroying the suspended frame
// must run its destructor. This is the contract M1's cancellation relies on.
Simple guarded()
{
    Guard g;
    co_await std::suspend_always{};
    co_return 1;
}

// Symmetric transfer: await_suspend hands control to another coroutine rather
// than returning to the resumer. Task<T>'s final_suspend is built on this.
struct Transfer {
    std::coroutine_handle<> to;

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<>) const noexcept { return to; }

    void await_resume() const noexcept {}
};

u32 tail_ran;

Simple tail()
{
    tail_ran++;
    co_return 0;
}

Simple head(std::coroutine_handle<> to)
{
    co_await Transfer{ to };
    co_return 0;
}

// Returning the handle out of an opaque call defeats the compiler's coroutine
// heap-allocation elision, so the frame really comes from the kernel heap.
__attribute__((noinline)) Simple escaping(u32 a, u32 b)
{
    return add(a, b);
}
} // namespace

void test_coroutine()
{
    test_begin("coroutine");

    // A suspended frame keeps its locals; the promise carries the result out.
    Simple s = add(20, 22);
    CHECK(bool(s.h));
    CHECK(!s.h.done());
    s.h.resume();
    CHECK(!s.h.done());
    s.h.resume();
    CHECK(s.h.done());
    CHECK_EQ(s.h.promise().value, 42);
    s.h.destroy();

    // Frames come from the kernel heap, and destroy() returns them.
    usize allocs_before = heap_stats().allocs;
    usize in_use_before = heap_stats().bytes_in_use;
    Simple f            = escaping(1, 1);
    CHECK(heap_stats().allocs > allocs_before);
    CHECK(heap_stats().bytes_in_use > in_use_before);
    f.h.destroy();
    CHECK_EQ(heap_stats().bytes_in_use, in_use_before);

    // Destroying a suspended coroutine unwinds it and runs its destructors.
    live_guards = 0;
    Simple g    = guarded();
    g.h.resume(); // into the body, then suspended at the co_await
    CHECK_EQ(live_guards, 1);
    g.h.destroy();
    CHECK_EQ(live_guards, 0);

    // Running one to completion also destroys the local.
    live_guards = 0;
    Simple g2   = guarded();
    g2.h.resume();
    g2.h.resume();
    CHECK(g2.h.done());
    CHECK_EQ(live_guards, 0);
    g2.h.destroy();

    // await_suspend returning a handle resumes that coroutine directly.
    tail_ran  = 0;
    Simple t  = tail();
    Simple hd = head(t.h);
    hd.h.resume();
    CHECK_EQ(tail_ran, 1);
    CHECK(t.h.done());
    CHECK(!hd.h.done());
    hd.h.destroy();
    t.h.destroy();

    // noop_coroutine is a valid handle that does nothing.
    auto noop = std::noop_coroutine();
    CHECK(bool(noop));
    CHECK(!noop.done());
    noop.resume();

    // A default-constructed handle is falsy and compares equal to nullptr.
    std::coroutine_handle<> empty;
    CHECK(!empty);
    CHECK(empty == nullptr);
}

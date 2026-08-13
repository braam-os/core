#include "harness.h"
#include "kernel/alloc.h"
#include "kernel/task.h"

namespace {

u32 body_ran;

Task<u32> answer()
{
    body_ran++;
    co_return 42;
}

Task<void> bump()
{
    body_ran++;
    co_return;
}

// Awaiting a child enters it by symmetric transfer and comes back with its
// value; the child's frame is destroyed with its Task at the end of the await.
Task<u32> nested()
{
    co_await bump();
    u32 v = co_await answer();
    co_return v + 1;
}

int live_guards;

struct Guard {
    Guard() { live_guards++; }

    ~Guard() { live_guards--; }
};

Task<u32> guarded()
{
    Guard g;
    co_await std::suspend_always{};
    co_return 0;
}

// Defeats the compiler's coroutine heap-allocation elision, so the frame
// really comes from the kernel heap.
__attribute__((noinline)) Task<u32> escaping()
{
    return answer();
}

} // namespace

void test_task()
{
    test_begin("task");

    // Lazy: nothing runs until the handle is resumed.
    body_ran    = 0;
    Task<u32> t = answer();
    CHECK(bool(t));
    CHECK(!t.done());
    CHECK_EQ(body_ran, 0);
    t.handle().resume();
    CHECK(t.done());
    CHECK_EQ(body_ran, 1);
    CHECK_EQ(t.handle().promise().value.value(), 42);

    // A void task carries no value, and a moved-from task owns nothing.
    Task<void> v     = bump();
    Task<void> moved = move(v);
    CHECK(!bool(v));
    CHECK(v.done());
    moved.handle().resume();
    CHECK(moved.done());

    // Chaining: one resume of the root runs the whole tree to completion.
    body_ran    = 0;
    Task<u32> n = nested();
    n.handle().resume();
    CHECK(n.done());
    CHECK_EQ(body_ran, 2);
    CHECK_EQ(n.handle().promise().value.value(), 43);

    // Frames come from the kernel heap and go back when the Task dies.
    usize in_use = heap_stats().bytes_in_use;
    {
        Task<u32> e = escaping();
        CHECK(heap_stats().bytes_in_use > in_use);
        e.handle().resume();
    }
    CHECK_EQ(heap_stats().bytes_in_use, in_use);

    // Destroying a suspended task runs the destructors of its locals.
    live_guards = 0;
    {
        Task<u32> g = guarded();
        g.handle().resume();
        CHECK_EQ(live_guards, 1);
    }
    CHECK_EQ(live_guards, 0);
}

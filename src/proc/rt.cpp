#include "rt.h"

#include "kernel/alloc.h"
#include "kernel/vec.h"

extern "C" void __wasm_call_ctors();

namespace {

// The whole scheduler. One task, one outstanding syscall, so a wake table
// would have nothing to look up.
struct Rt {
    Task<i32> root;
    std::coroutine_handle<> waiting;
    u32 token = 1;
    Vec<Str> argv;
    SysReply reply;
    String reply_data;
    bool started = false;
    bool exited  = false;
};

// The heap is up before _start, because _alloc is: the host places argv in
// this memory before it enters the program.
void ready()
{
    static bool done = false;
    if (!done) {
        done = true;
        heap_init(0);
        __wasm_call_ctors();
    }
}

Rt &rt()
{
    // Function-local rather than namespace-scope: a Task has a destructor, and
    // a namespace-scope global with one would need __cxa_atexit. The storage
    // is a byte array constructed in place on first use, the same trick Sched
    // uses (Concept.md §3.2).
    alignas(Rt) static u8 storage[sizeof(Rt)];
    static bool built = false;
    if (!built) {
        new (storage) Rt();
        built = true;
    }
    return *reinterpret_cast<Rt *>(storage);
}

// 0 = exited, 1 = suspended. Concept.md §4.3's return convention, and the only
// thing the kernel learns without a syscall.
i32 status_of(Rt &r)
{
    if (r.root && !r.root.done())
        return 1;
    if (!r.exited) {
        r.exited = true;
        i32 code = 1;
        if (r.root && r.root.handle().promise().value.has_value())
            code = r.root.handle().promise().value.value();
        sys(u32(Sys::Exit), u32(code), 0, 0);
    }
    return 0;
}

} // namespace

// host.h declares this; src/kernel/panic.cpp is the kernel's half. A process
// has no imports but the two syscalls, so a fatal error is a trap, which the
// kernel reports as a crashed process. The metadata `exec` reads is not here
// either: tools/stamp.py appends it after the link, where the page counts and
// the link flags are known in one place.
[[noreturn]] void panic_raw(const char *, usize)
{
    __builtin_trap();
}

void SysCall::await_suspend(std::coroutine_handle<> h)
{
    Rt &r     = rt();
    r.waiting = h;
    r.token++;
    sys_async(op_, r.token, proc_addr(payload_.data()), u32(payload_.size()));
}

Result<SysReply> SysCall::await_resume() const
{
    const SysReply &v = rt().reply;
    if (v.status < 0)
        return Err(Error(-v.status));
    return SysReply{ v.status, v.data };
}

BRAAM_EXPORT("_alloc") u32 _alloc(u32 n)
{
    ready();
    return u32(reinterpret_cast<usize>(heap_alloc(n)));
}

BRAAM_EXPORT("_free") void _free(u32 ptr, u32)
{
    heap_free(reinterpret_cast<void *>(usize(ptr)));
}

// The host has already written the argv blob at `ptr` through _alloc. The
// blob stays for the process's lifetime, because argv is a span of views into
// it and a program may hold those to the end.
BRAAM_EXPORT("_start") i32 _start(u32 ptr, u32 len)
{
    ready();

    Rt &r = rt();
    if (r.started)
        return 0;
    r.started = true;

    const u8 *blob = reinterpret_cast<const u8 *>(usize(ptr));
    usize n        = argv_count(blob, len);
    if (r.argv.reserve(n))
        for (usize i = 0; i < n; i++)
            r.argv.push(argv_at(blob, len, i));

    // A frame that would not allocate leaves the task null, and status_of
    // reports the failure as exit status 1 rather than resuming nothing.
    r.root = proc_main(Args{ Span<const Str>(r.argv.data(), r.argv.size()) });
    if (r.root)
        r.root.handle().resume();
    return status_of(r);
}

// The reply is a block the host allocated through _alloc: an i32 status, then
// any data. It is copied out and freed here rather than adopted, so a program
// holding the previous reply's view is not looking at a freed block.
BRAAM_EXPORT("_resume") i32 _resume(u32 token, u32 ptr, u32 len)
{
    Rt &r = rt();
    if (!r.waiting || token != r.token)
        return status_of(r);

    const u8 *p = reinterpret_cast<const u8 *>(usize(ptr));
    r.reply     = SysReply{};
    r.reply_data.clear();
    if (len >= 4) {
        r.reply.status = i32(sys_get_u32(p));
        if (len > 4 && r.reply_data.assign(Str(reinterpret_cast<const char *>(p + 4), len - 4)))
            r.reply.data = r.reply_data.str();
    } else {
        r.reply.status = -i32(Error::Io);
    }
    heap_free(reinterpret_cast<void *>(usize(ptr)));

    std::coroutine_handle<> h = r.waiting;
    r.waiting                 = nullptr;
    h.resume();
    return status_of(r);
}

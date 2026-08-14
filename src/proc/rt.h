// The process-side runtime: a separate binary's whole libc (Concept.md §4.3,
// §4.4). It is deliberately small, because every binary carries its own copy —
// anything substantial belongs in a syscall, where it lives once in the kernel.
//
// A process has a handful of tasks and one outstanding syscall each, so the
// scheduler here is two small arrays rather than the kernel's queues and wake
// table. Nothing cancels from inside: the kernel kills a process by dropping
// the instance, so a killed process never unwinds.
#pragma once

#include "kernel/coroutine.h"
#include "kernel/result.h"
#include "kernel/span.h"
#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/sysabi.h"
#include "kernel/task.h"
#include "kernel/types.h"

// Names the process's imports. They are the kernel's, not the host's, and the
// pid the kernel dispatches on is bound by the closure rather than passed —
// which is what makes a syscall on another process's behalf unrepresentable.
#define BRAAM_SYS_IMPORT(name) \
    extern "C" __attribute__((import_module("kernel"), import_name(name)))

BRAAM_SYS_IMPORT("sys") i32 sys(u32 op, u32 a0, u32 a1, u32 a2);
BRAAM_SYS_IMPORT("sys_async") void sys_async(u32 op, u32 token, u32 ptr, u32 len);

// argv, the same shape src/user/prog.h gives a shell builtin.
struct Args {
    Span<const Str> v;

    usize size() const { return v.size(); }

    Str operator[](usize i) const { return v[i]; }

    Str name() const { return v.empty() ? Str() : v[0]; }

    Args tail() const { return Args{ v.subspan(1) }; }
};

// What a program defines. The runtime's _start builds argv and enters it.
Task<i32> proc_main(Args args);

// A wasm32 pointer as the integer the syscall wire carries.
inline u32 proc_addr(const void *p)
{
    return u32(reinterpret_cast<usize>(p));
}

inline u32 proc_pid()
{
    return u32(sys(u32(Sys::GetPid), 0, 0, 0));
}

inline u32 proc_now()
{
    return u32(sys(u32(Sys::Now), 0, 0, 0));
}

// One asynchronous syscall's answer: the status the kernel chose, and the
// bytes it sent, which stay valid until the next syscall.
struct SysReply {
    i32 status = 0;
    Str data;
};

// How many tasks a process may have at once, and therefore how many syscalls
// it may have outstanding: one each. Four, because the one program that needs
// more than the root task needs exactly one more, and a table this small costs
// bytes rather than a design.
constexpr usize PROC_TASKS = 4;

// A second task, for a program that must await two things at once — `chat`
// listens to a socket while it reads what is typed. The task starts at once and
// runs until its first suspension. Reports 0 when the table is full.
//
// The process ends when *the root task* returns, whatever the others are doing,
// exactly as a process ends when main does: the kernel then drops the instance
// and fails any request the others had outstanding.
u32 proc_spawn(Task<i32> t);

// Hands a payload to the kernel and suspends the task until _resume brings the
// answer back. Used as an awaiter directly, so the payload is still on the
// caller's stack when the host copies it out.
struct SysCall {
    SysCall(u32 op, Str payload) : op_(op), payload_(payload) {}

    SysCall(const SysCall &)            = delete;
    SysCall &operator=(const SysCall &) = delete;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h);

    Result<SysReply> await_resume() const;

private:
    u32 op_;
    Str payload_;
    usize slot_ = 0; // which waiter await_suspend took; await_resume reads it
};

inline SysCall sys_call(Sys op, u32 fd, Str payload = Str())
{
    return SysCall{ sys_op(op, fd), payload };
}

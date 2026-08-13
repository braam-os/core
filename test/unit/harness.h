// Assertions that report through host_log. Cases are listed explicitly in
// main.cpp, which fixes the order some of them depend on.
#pragma once

#include "kernel/host.h"
#include "kernel/str.h"
#include "kernel/task.h"
#include "kernel/traits.h"
#include "kernel/types.h"

// Runs a task that is known not to suspend and takes its value. Every
// filesystem the unit tests mount is in memory, so nothing they call parks;
// the asynchronous path is the smoke test's to prove, against a host.
template <class T>
T run_now(Task<T> t)
{
    if (!t)
        panic("run_now: the frame would not allocate");
    t.handle().resume();
    if (!t.done())
        panic("run_now: the task suspended");
    return move(t.handle().promise().value.value());
}

void test_begin(Str name);
void test_check(bool ok, Str expr, Str file, u32 line);
void test_check_eq(u32 a, u32 b, Str expr, Str file, u32 line);
u32 test_failures();

#define CHECK(expr)    test_check((expr), #expr, __FILE_NAME__, __LINE__)
#define CHECK_EQ(a, b) test_check_eq(u32(a), u32(b), #a " == " #b, __FILE_NAME__, __LINE__)

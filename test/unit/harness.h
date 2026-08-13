// Assertions that report through host_log. Cases are listed explicitly in
// main.cpp, which fixes the order some of them depend on.
#pragma once

#include "kernel/str.h"
#include "kernel/types.h"

void test_begin(Str name);
void test_check(bool ok, Str expr, Str file, u32 line);
void test_check_eq(u32 a, u32 b, Str expr, Str file, u32 line);
u32 test_failures();

#define CHECK(expr) test_check((expr), #expr, __FILE_NAME__, __LINE__)
#define CHECK_EQ(a, b) test_check_eq(u32(a), u32(b), #a " == " #b, __FILE_NAME__, __LINE__)

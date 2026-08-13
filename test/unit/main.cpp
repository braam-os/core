// tests.wasm — the same kernel sources, driven from Node. Kept out of
// kernel.wasm so test code never counts against the size budget.

#include "harness.h"

#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/host.h"

void test_alloc();
void test_str();
void test_vec();
void test_result();
void test_coroutine();
void test_fmt();

// Returns the number of failed checks; the harness treats nonzero as failure.
BRAAM_EXPORT("run_tests") u32 run_tests() {
    heap_init(0);

    test_str();
    test_fmt();
    test_result();
    test_alloc();
    test_vec();
    test_coroutine();

    u32 failures = test_failures();
    HeapStats s = heap_stats();
    Buf<160> line;
    line.put(failures ? "FAILED: " : "ok: ")
        .put(failures)
        .put(" failures, ")
        .put(u32(s.allocs))
        .put(" allocs / ")
        .put(u32(s.frees))
        .put(" frees, ")
        .put(u32(s.bytes_reserved >> 10))
        .put(" KiB reserved");
    log(line.str());
    return failures;
}

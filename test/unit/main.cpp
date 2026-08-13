// tests.wasm — the same kernel sources, driven from Node. Kept out of
// kernel.wasm so test code never counts against the size budget.

#include "harness.h"

#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/host.h"

void test_alloc();
void test_str();
void test_string();
void test_vec();
void test_hash();
void test_result();
void test_coroutine();
void test_task();
void test_sched();
void test_fmt();
void test_channel();
void test_screen();
void test_text();
void test_prog();
void test_edit();
void test_tokenize();
void test_shell();

// The kernel's init() calls this too: it is what populates the program
// registry, so the cases below see the same set of programs kernel.wasm does.
extern "C" void __wasm_call_ctors();

// Returns the number of failed checks; the harness treats nonzero as failure.
BRAAM_EXPORT("run_tests") u32 run_tests() {
    heap_init(0);
    __wasm_call_ctors();

    test_str();
    test_fmt();
    test_result();
    test_alloc();
    test_vec();
    test_string();
    test_hash();
    test_coroutine();
    test_task();
    test_sched();
    test_channel();
    test_screen();
    test_text(); // after screen: it round-trips through the grid
    test_prog();
    test_edit();
    test_tokenize();
    test_shell();

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

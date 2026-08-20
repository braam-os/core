// tests.wasm — the same kernel sources, driven from Node. Kept out of
// kernel.wasm so test code never counts against the size budget.

#include "harness.h"
#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/host.h"
#include "kernel/hostcall.h"
#include "kernel/jsref.h"
#include "kernel/sched.h"

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
void test_io();
void test_screen();
void test_tty();
void test_cond();
void test_console();
void test_encode();
void test_sha256();
void test_version();
void test_db();
void test_dep();
void test_stanza();
void test_trust();
void test_text();
void test_procfs();
void test_pane();
void test_textbuf();
void test_time();
void test_tokenize();
void test_parse();
void test_expand();
void test_match();
void test_opt();
void test_path();
void test_hostfs();
void test_jsref();
void test_svc();
void test_sysabi();
void test_vfs();
void test_zip();

// The kernel's init() calls this too, so the cases below start from the same
// static state kernel.wasm does.
extern "C" void __wasm_call_ctors();

// The same wake() kernel.wasm exports. The storage fake in test/fakefs.mjs
// answers from inside the import, so a case that boots the shell needs a way
// back in; wake only queues a resumption, so the tick already on the stack
// picks it up on its way out.
BRAAM_EXPORT("wake") void wake(u32 token, u32 payload_ptr, u32 payload_len)
{
    if (!sched_wake(token, payload_ptr, payload_len))
        host_orphan_reply(token);
}

// The same ref() kernel.wasm exports, so the service fake can hand objects in.
BRAAM_EXPORT("ref") void ref(u32 slot, __externref_t obj)
{
    jsref_set(slot, obj);
}

// Returns the number of failed checks; the harness treats nonzero as failure.
BRAAM_EXPORT("run_tests") u32 run_tests()
{
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
    test_io();
    test_screen();
    test_text();    // after screen: it round-trips through the grid
    test_tty();     // after screen: FullScreen snapshots the grid
    test_console(); // after tty: the pump routes through its claims
    test_pane();
    test_textbuf();
    test_time();
    test_path();
    test_jsref();
    test_hostfs();
    test_svc();
    test_sysabi();
    test_vfs();
    test_tokenize();
    test_parse();
    test_expand();
    test_match();
    test_cond();
    test_opt();
    test_encode();
    test_sha256(); // after encode: the vectors are compared as hex
    test_version();
    test_dep(); // after version: a dependency is a mask over a comparison
    test_stanza();
    test_trust(); // after stanza and svc: the anchor over both
    test_db();    // after dep: world is a list of dependency tokens
    test_zip(); // after sha256 and svc: rootfs.zip is compared by digest
    test_procfs();

    u32 failures = test_failures();
    HeapStats s  = heap_stats();
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

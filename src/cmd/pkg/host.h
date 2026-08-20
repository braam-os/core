// The host operations trust.cpp and index.cpp need, as an interface, so that
// both compile into tests.wasm: /bin/pkg answers with syscalls and the unit
// suite with the kernel's own services.
//
// No virtual destructor: nothing is deleted through the base, and a
// namespace-scope global must stay trivially destructible.
#pragma once

#include "kernel/result.h"
#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/task.h"
#include "kernel/types.h"

struct PkgHost {
    // The wall clock, milliseconds since the epoch. Read once per run (§7).
    virtual Task<Result<u64>> now() = 0;

    // A whole small file. Err(NotFound) when there is none.
    virtual Task<Result<String>> load(Str path) = 0;

    // A fetch. The token names the body; `status` is the HTTP status.
    virtual Task<Result<i32>> open(Str url, u32 &status) = 0;

    // One chunk of a body, Err(Closed) at the end.
    virtual Task<Result<String>> read(i32 body) = 0;

    virtual Task<void> close(i32 body) = 0;

    // Ed25519: true is a good signature, false a bad one, an Err a fault.
    virtual Task<Result<bool>> verify(Str key, Str sig, Str bytes) = 0;
};

// What /bin/pkg performs them with. Defined in host.cpp, which stays out of
// tests.wasm.
PkgHost &pkg_host();

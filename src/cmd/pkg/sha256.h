// SHA-256, FIPS 180-4. Streaming, so a body is hashed as it arrives.
#pragma once

#include "kernel/span.h"
#include "kernel/str.h"
#include "kernel/types.h"

constexpr usize SHA256_SIZE = 32;

// 112 bytes: a coroutine streaming a body keeps this in a heap record, not in
// its frame.
struct Sha256 {
    Sha256() { reset(); }

    void reset();

    void update(Bytes in);

    void update(Str s) { update(Bytes(reinterpret_cast<const u8 *>(s.data()), s.size())); }

    // Pads and writes the digest. reset() to hash again.
    void finish(u8 out[SHA256_SIZE]);

private:
    void compress();

    u32 h_[8];
    u8 block_[64];
    u64 len_    = 0; // bytes seen
    usize fill_ = 0;
};

void sha256(Bytes in, u8 out[SHA256_SIZE]);

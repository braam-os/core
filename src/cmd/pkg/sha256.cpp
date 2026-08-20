#include "sha256.h"

namespace {

constexpr u32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr u32 ror(u32 x, u32 n)
{
    return (x >> n) | (x << (32 - n));
}

// Big-endian, by hand: wasm is little-endian and there is no htonl here.
constexpr u32 load_be(const u8 *p)
{
    return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | u32(p[3]);
}

void store_be(u8 *p, u32 v)
{
    p[0] = u8(v >> 24);
    p[1] = u8(v >> 16);
    p[2] = u8(v >> 8);
    p[3] = u8(v);
}

} // namespace

void Sha256::reset()
{
    h_[0] = 0x6a09e667;
    h_[1] = 0xbb67ae85;
    h_[2] = 0x3c6ef372;
    h_[3] = 0xa54ff53a;
    h_[4] = 0x510e527f;
    h_[5] = 0x9b05688c;
    h_[6] = 0x1f83d9ab;
    h_[7] = 0x5be0cd19;
    len_  = 0;
    fill_ = 0;
}

void Sha256::compress()
{
    u32 w[64];
    for (usize i = 0; i < 16; i++)
        w[i] = load_be(block_ + i * 4);
    for (usize i = 16; i < 64; i++) {
        u32 s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i]   = w[i - 16] + s0 + w[i - 7] + s1;
    }

    u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    u32 e = h_[4], f = h_[5], g = h_[6], h = h_[7];

    for (usize i = 0; i < 64; i++) {
        u32 s1    = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        u32 ch    = (e & f) ^ (~e & g);
        u32 temp1 = h + s1 + ch + K[i] + w[i];
        u32 s0    = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        u32 maj   = (a & b) ^ (a & c) ^ (b & c);
        u32 temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(Bytes in)
{
    len_ += in.size();
    for (usize at = 0; at < in.size();) {
        usize take = sizeof(block_) - fill_;
        if (take > in.size() - at)
            take = in.size() - at;
        __builtin_memcpy(block_ + fill_, in.data() + at, take);
        fill_ += take;
        at += take;
        if (fill_ == sizeof(block_)) {
            compress();
            fill_ = 0;
        }
    }
}

void Sha256::finish(u8 out[SHA256_SIZE])
{
    u64 bits        = len_ * 8;
    block_[fill_++] = 0x80;
    if (fill_ > sizeof(block_) - 8) {
        while (fill_ < sizeof(block_))
            block_[fill_++] = 0;
        compress();
        fill_ = 0;
    }
    while (fill_ < sizeof(block_) - 8)
        block_[fill_++] = 0;
    store_be(block_ + 56, u32(bits >> 32));
    store_be(block_ + 60, u32(bits));
    compress();

    for (usize i = 0; i < 8; i++)
        store_be(out + i * 4, h_[i]);
}

void sha256(Bytes in, u8 out[SHA256_SIZE])
{
    Sha256 s;
    s.update(in);
    s.finish(out);
}

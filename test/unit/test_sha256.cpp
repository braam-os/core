#include "cmd/pkg/encode.h"
#include "cmd/pkg/sha256.h"
#include "harness.h"

namespace {

// The digest as lowercase hex, which is how the vectors are written.
struct Digest {
    char text[hex_size(SHA256_SIZE)];

    Str str() const { return Str(text, sizeof(text)); }
};

Digest hex_of(const u8 d[SHA256_SIZE])
{
    Digest out;
    hex_encode(Bytes(d, SHA256_SIZE), Span<char>(out.text));
    return out;
}

Digest digest_of(Str s)
{
    u8 d[SHA256_SIZE];
    sha256(Bytes(reinterpret_cast<const u8 *>(s.data()), s.size()), d);
    return hex_of(d);
}

// The same input, fed `step` bytes at a time.
Digest chunked(Str s, usize step)
{
    Sha256 h;
    for (usize at = 0; at < s.size(); at += step) {
        usize take = step < s.size() - at ? step : s.size() - at;
        h.update(s.substr(at, take));
    }
    u8 d[SHA256_SIZE];
    h.finish(d);
    return hex_of(d);
}

constexpr Str EMPTY_DIGEST = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr Str ABC          = "abc";
constexpr Str ABC_DIGEST   = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

// 56 bytes: the padding does not fit and spills into a second block.
constexpr Str TWO_BLOCK        = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
constexpr Str TWO_BLOCK_DIGEST = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";

// 112 bytes, two full blocks and a third for the padding.
constexpr Str LONG =
    "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
    "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
constexpr Str LONG_DIGEST = "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1";

} // namespace

void test_sha256()
{
    test_begin("sha256");

    CHECK(digest_of("").str() == EMPTY_DIGEST);
    CHECK(digest_of(ABC).str() == ABC_DIGEST);
    CHECK(digest_of(TWO_BLOCK).str() == TWO_BLOCK_DIGEST);
    CHECK_EQ(TWO_BLOCK.size(), 56);
    CHECK(digest_of(LONG).str() == LONG_DIGEST);
    CHECK_EQ(LONG.size(), 112);

    // One million 'a', as a thousand updates: the only vector that moves the
    // length counter past what a block index would hold.
    {
        char a[1000];
        for (usize i = 0; i < sizeof(a); i++)
            a[i] = 'a';
        Sha256 h;
        for (usize i = 0; i < 1000; i++)
            h.update(Str(a, sizeof(a)));
        u8 d[SHA256_SIZE];
        h.finish(d);
        CHECK(hex_of(d).str() ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    // Every relation between a chunk boundary and the 64-byte block.
    constexpr usize STEPS[] = { 1, 2, 63, 64, 65, 127 };
    for (usize step : STEPS) {
        CHECK(chunked("", step).str() == EMPTY_DIGEST);
        CHECK(chunked(ABC, step).str() == ABC_DIGEST);
        CHECK(chunked(TWO_BLOCK, step).str() == TWO_BLOCK_DIGEST);
        CHECK(chunked(LONG, step).str() == LONG_DIGEST);
    }

    // reset() puts a used context back where it started.
    {
        Sha256 h;
        u8 d[SHA256_SIZE];
        h.update(LONG);
        h.finish(d);
        h.reset();
        h.finish(d);
        CHECK(hex_of(d).str() == EMPTY_DIGEST);
    }
}

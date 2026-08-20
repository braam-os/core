#include "cmd/pkg/encode.h"
#include "harness.h"

namespace {

Bytes bytes_of(Str s)
{
    return Bytes(reinterpret_cast<const u8 *>(s.data()), s.size());
}

// Encodes and compares in one place, so a case is one line.
bool b64_is(Str in, Str want)
{
    char out[64];
    if (!base64_encode(bytes_of(in), Span<char>(out)))
        return false;
    return Str(out, base64_size(in.size())) == want;
}

bool b64_back(Str in, Str want)
{
    u8 out[64];
    Option<usize> n = base64_decode(in, Span<u8>(out));
    return n && Str(reinterpret_cast<const char *>(out), n.value()) == want;
}

bool b64_refused(Str in)
{
    u8 out[64];
    return !base64_decode(in, Span<u8>(out));
}

} // namespace

void test_encode()
{
    test_begin("encode");

    // RFC 4648 §10, both ways.
    CHECK(b64_is("", ""));
    CHECK(b64_is("f", "Zg=="));
    CHECK(b64_is("fo", "Zm8="));
    CHECK(b64_is("foo", "Zm9v"));
    CHECK(b64_is("foob", "Zm9vYg=="));
    CHECK(b64_is("fooba", "Zm9vYmE="));
    CHECK(b64_is("foobar", "Zm9vYmFy"));

    CHECK(b64_back("", ""));
    CHECK(b64_back("Zg==", "f"));
    CHECK(b64_back("Zm8=", "fo"));
    CHECK(b64_back("Zm9v", "foo"));
    CHECK(b64_back("Zm9vYg==", "foob"));
    CHECK(b64_back("Zm9vYmE=", "fooba"));
    CHECK(b64_back("Zm9vYmFy", "foobar"));

    // Both ends of the alphabet, and a digest-sized value.
    {
        u8 all[256];
        for (usize i = 0; i < sizeof(all); i++)
            all[i] = u8(i);

        char enc[base64_size(sizeof(all))];
        CHECK(base64_encode(Bytes(all), Span<char>(enc)));

        u8 back[sizeof(all)];
        Option<usize> n = base64_decode(Str(enc, sizeof(enc)), Span<u8>(back));
        CHECK(n.has_value());
        CHECK_EQ(n ? n.value() : 0, sizeof(all));
        bool same = true;
        for (usize i = 0; i < sizeof(all); i++)
            same = same && back[i] == all[i];
        CHECK(same);
    }

    // Strict, every way it can fail.
    CHECK(b64_refused("Zm9vYmFy="));        // not a multiple of four
    CHECK(b64_refused("Zg="));              // ditto, shorter
    CHECK(b64_refused("Zm9*"));             // outside the alphabet
    CHECK(b64_refused("Zm 9v"));            // whitespace is not skipped
    CHECK(b64_refused("Z=g="));             // padding in the middle
    CHECK(b64_refused("=g=="));             // padding at the front
    CHECK(b64_refused("Z==="));             // three pads
    CHECK(b64_refused("Zm9vYmFyZg==Zg==")); // padding before the last group
    CHECK(b64_refused("Zh=="));             // the dropped byte's bits are not zero
    CHECK(b64_refused("Zm9="));             // ditto, one pad

    // An output buffer one byte short.
    {
        u8 out[2];
        CHECK(!base64_decode("Zm9v", Span<u8>(out)));
        char enc[3];
        CHECK(!base64_encode(bytes_of("foo"), Span<char>(enc)));
    }

    // Hex: lowercase out, either case in.
    {
        u8 in[4] = { 0x00, 0x0f, 0xa5, 0xff };
        char out[hex_size(sizeof(in))];
        CHECK(hex_encode(Bytes(in), Span<char>(out)));
        CHECK(Str(out, sizeof(out)) == "000fa5ff");

        u8 back[4];
        CHECK_EQ(hex_decode("000FA5ff", Span<u8>(back)).value(), sizeof(back));
        bool same = true;
        for (usize i = 0; i < sizeof(in); i++)
            same = same && back[i] == in[i];
        CHECK(same);

        CHECK(!hex_decode("abc", Span<u8>(back)));  // odd
        CHECK(!hex_decode("00gg", Span<u8>(back))); // not a digit
        CHECK(!hex_decode("00 0f", Span<u8>(back)));
        CHECK_EQ(hex_decode("", Span<u8>(back)).value(), 0);

        u8 small[1];
        CHECK(!hex_decode("0011", Span<u8>(small)));
        char narrow[7];
        CHECK(!hex_encode(Bytes(in), Span<char>(narrow)));
    }
}

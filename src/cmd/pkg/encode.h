// Hex and base64, both ways. Base64 is the wire encoding of every digest, key
// and signature (Package_Formats.md §1.1); hex is a fingerprint a person types.
//
// Nothing here allocates: the caller owns the buffer, and the largest thing pkg
// encodes is a 64-byte signature.
#pragma once

#include "kernel/result.h"
#include "kernel/span.h"
#include "kernel/str.h"
#include "kernel/types.h"

constexpr usize hex_size(usize bytes)
{
    return bytes * 2;
}

constexpr usize base64_size(usize bytes)
{
    return ((bytes + 2) / 3) * 4;
}

// Lowercase. False when `out` is shorter than hex_size(in.size()).
bool hex_encode(Bytes in, Span<char> out);

// Either case. None on an odd length, a non-hex character, or an `out` too
// small; a rejection writes nothing usable.
Option<usize> hex_decode(Str in, Span<u8> out);

// Standard alphabet, padded. False when `out` is too small.
bool base64_encode(Bytes in, Span<char> out);

// Strict: a length that is a multiple of four, `=` only as the last one or two
// characters, no character outside the alphabet, and the unused bits of a short
// final group zero — two spellings must not decode alike, or a key name can be
// claimed twice (Package_Formats.md §2).
Option<usize> base64_decode(Str in, Span<u8> out);

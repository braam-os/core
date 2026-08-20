// The anchor, checked (Package_Format.md §4, Package_Management.md §7 step 4
// and §10). The host is a parameter, so nothing here needs a syscall.
//
// A refusal is false, a fault an Err.
#pragma once

#include "host.h"
#include "kernel/result.h"
#include "kernel/span.h"
#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/task.h"
#include "kernel/types.h"
#include "kernel/vec.h"
#include "stanza.h"

// One algorithm, no negotiation (Package_Management.md §8).
constexpr Str TRUST_ALGORITHM = "ed25519";

// §4's two uses; any other is ignored.
constexpr Str TRUST_ROOT  = "root";
constexpr Str TRUST_INDEX = "index";

constexpr u32 TRUST_GRAMMAR = 1;

// Shipped in rootfs.zip, re-pinned by the boot unpack (§6).
constexpr Str ANCHOR_PATH = "/share/pkg/anchor";

// One anchor: the text it owns, §2's split of it, and the record.
struct AnchorFile {
    String text;
    Str block; // the Y: lines
    Str body;  // the signed bytes
    Vec<Signature> sigs;
    Anchor rec;
};

// §1.1: the Q2 digest of `<algorithm> <base64 key>`, recomputed, never read.
bool trust_key_name(const AnchorKey &k, Span<char> out);

// The grammar, then §4's rest: X is this grammar, H comes once per use, K
// once, an ed25519 key is 32 bytes. Takes `text`, which every Str then views.
bool anchor_file_read(String text, AnchorFile &out);

// The count `use` needs, or 0, which nothing meets.
u32 trust_threshold(const Anchor &a, Str use);

// §7 step 4: signatures over `body` by the keys `use` names, **at most one per
// key**, against that use's threshold.
Task<Result<bool>> trust_meet(const Anchor &keys, Str use, const Vec<Signature> &sigs, Str body,
                              PkgHost &h);

// An anchor against itself: unexpired at `now`, and its own root threshold.
Task<Result<bool>> trust_self(const AnchorFile &a, u64 now, PkgHost &h);

// §10: `next` needs a G past `cur`'s and a threshold of `cur`'s root keys as
// well as one of its own.
Task<Result<bool>> trust_step(const AnchorFile &cur, const AnchorFile &next, u64 now, PkgHost &h);

// Forward from a `chain[0]` the caller has checked, while each step holds. The
// answer is the index reached, 0 when none was adopted.
Task<Result<usize>> trust_walk(Span<const AnchorFile> chain, u64 now, PkgHost &h);

// §7 step 2: missing is Err(NotFound), unreadable Err(Invalid), one that does
// not meet its own root threshold at `now` Err(Perm). All three stop.
Task<Result<void>> anchor_load(PkgHost &h, u64 now, AnchorFile &out);

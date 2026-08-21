// The stanza grammar (Package_Format.md §1) and the records §3, §4 and §8.1
// define over it. One reader for all five files.
//
// Every Str views the text the caller holds; nothing here copies a value.
#pragma once

#include "encode.h"
#include "kernel/result.h"
#include "kernel/span.h"
#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/types.h"
#include "kernel/vec.h"
#include "sha256.h"

// ------------------------------------------------------------ §1, the lines

// StanzaField, not Field: a name at global scope must be unique across the
// tree, and src/cmd/sh/expand.h already has a Field. Two of them make
// Vec<Field> one symbol over two layouts.
struct StanzaField {
    char letter = 0;
    Str value;
};

enum class StanzaRead {
    Ok,
    Unusable,  // this record, not the file
    Malformed, // the file does not read
    End,
};

// Every letter a file may carry. An unknown uppercase one makes the record
// unusable, an unknown lowercase one is dropped.
constexpr Str STANZA_SIGNATURE = "Y";                  // §2
constexpr Str STANZA_HEADER    = "XNGET";              // §3.1
constexpr Str STANZA_PACKAGE   = "CPVSITDpiotkg";      // §3.2, and .PKGINFO
constexpr Str STANZA_ANCHOR    = "XGEHK";              // §4
constexpr Str STANZA_DB        = "CPVSITDpiotkgGbFRZ"; // §8.1

// Y, K, H, F, R and Z repeat; every other letter is malformed twice.
constexpr Str STANZA_ACCUMULATING = "YKHFRZ";

struct StanzaReader {
    StanzaReader(Str text, Str known) : rest_(text), known_(known) {}

    StanzaReader(const StanzaReader &)            = delete;
    StanzaReader &operator=(const StanzaReader &) = delete;

    // Fills `out` with one stanza's fields, in the order they appeared.
    StanzaRead next(Vec<StanzaField> &out);

    // The whole of `text`, as one stanza of `known` and no second.
    static bool one(Str text, Str known, Vec<StanzaField> &out);

private:
    Str rest_;
    Str known_;
};

// ------------------------------------------------------- §1.1, the scalars

// Decimal, unsigned, unpadded: 007, +1, 1a and an overflow are all None.
Option<u64> stanza_number(Str s);

// Q2 and nothing else: base64 of 32 bytes, canonical.
bool digest_parse(Str s, u8 out[SHA256_SIZE]);

constexpr usize DIGEST_TEXT = 2 + base64_size(SHA256_SIZE);

bool digest_write(const u8 d[SHA256_SIZE], Span<char> out);

// ------------------------------------------------------------ §2, the split

// The signature block, and the signed bytes: everything after the first empty
// line. False when there is no empty line.
bool signed_split(Str text, Str &block, Str &body);

// ----------------------------------------------------------- the records

struct Signature {
    Str algorithm;
    Str key_name;
    Str signature;
};

struct IndexHeader {
    u32 grammar = 0; // X
    Str url;         // N
    u64 version = 0; // G
    u64 expiry  = 0; // E
    Str description; // T
};

struct PackageStanza {
    u8 digest[SHA256_SIZE] = {}; // C
    Str name;                    // P
    Str version;                 // V
    u64 size           = 0;      // S
    u64 installed_size = 0;      // I
    Str description;             // T
    Str depends;                 // D — a §6 list, its consumer's to split
    Str provides;                // p
    Str install_if;              // i
    Str origin;                  // o
    u64 build_time = 0;          // t
    u32 priority   = 0;          // k
    Str globs;                   // g
};

struct AnchorThreshold {
    Str use;
    u32 count = 0;
};

struct AnchorKey {
    Str use;
    Str algorithm;
    Str key;
};

struct Anchor {
    u32 grammar = 0;
    u64 version = 0;
    u64 expiry  = 0;
    Vec<AnchorThreshold> thresholds;
    Vec<AnchorKey> keys;
};

struct DbFile {
    Str dir;
    Str name;
    u8 digest[SHA256_SIZE] = {};
};

struct DbRecord {
    PackageStanza pkg;
    u64 index_version = 0; // G
    Str broken;            // b — the script that failed, empty when none did
    Vec<DbFile> files;
};

// Ok or Unusable: a missing required field or one that does not parse is the
// record's problem, not the file's.
StanzaRead signature_read(const Vec<StanzaField> &f, Vec<Signature> &out);
StanzaRead header_read(const Vec<StanzaField> &f, IndexHeader &out);
StanzaRead package_read(const Vec<StanzaField> &f, PackageStanza &out);
StanzaRead anchor_read(const Vec<StanzaField> &f, Anchor &out);
StanzaRead db_read(const Vec<StanzaField> &f, DbRecord &out);

// ------------------------------------------------------------- the writer

// §3.3's order, appended to `out`. False only on an allocation failure.
bool package_write(const PackageStanza &p, String &out);

// §3.3's order, then G, then b, then each F with its R and Z pairs.
bool db_write(const DbRecord &r, String &out);

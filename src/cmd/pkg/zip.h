// A package is a zip: §5.2 is parseZip's rules (web/fs.js) written down once,
// §5.1 the top-level dot-entry split (Package_Format.md). unzip.h is the half
// that inflates; nothing here needs a syscall.
//
// Every Str views the archive the caller holds.
#pragma once

#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/sysabi.h"
#include "kernel/types.h"
#include "kernel/vec.h"

// ------------------------------------------------------------ §5.2, the zip

constexpr u32 ZIP_STORE   = 0;
constexpr u32 ZIP_DEFLATE = 8;

struct ZipEntry {
    Str name;
    Str data; // where the *local* header says, never the central one
    u32 method = ZIP_STORE;
    u64 size   = 0; // the declared uncompressed size: the inflate's ceiling
};

enum class ZipRead {
    Ok,
    Malformed,   // not a zip this reader reads
    Unsupported, // zip64, an encrypted entry, or a method that is not 0 or 8
    NoMemory,
};

// The most an entry may be compressed to: Sys::Inflate stages its input, so a
// larger one cannot be read at all.
constexpr usize ZIP_PACKED_MAX = SYS_STAGE_MAX;

// The central directory, in its own order. A name ending in `/` is skipped.
ZipRead zip_entries(Str archive, Vec<ZipEntry> &out);

// Method 0: the archive's own bytes, once the declared size agrees.
bool zip_stored(const ZipEntry &e, Str &out);

// ---------------------------------------------------- §5.1, the dot-entries

// Only a name with no `/` can be metadata, so bin/.keep is payload.
enum class ZipMeta {
    Payload,
    PkgInfo,
    PreInstall,
    PostInstall,
    PreDeinstall,
    PostDeinstall,
    PreUpgrade,
    PostUpgrade,
    Trigger,
    Unknown, // a dot-entry that is none of these: the package is uninstallable
};

ZipMeta zip_meta(Str name);

// --------------------------------------------------------------- the ceiling

// An inflate's output side. Sys::Inflate caps its input and not its output, so
// the declared size is what stops a bomb.
struct ZipSink {
    explicit ZipSink(u64 want) : want_(want) {}

    ZipSink(const ZipSink &)            = delete;
    ZipSink &operator=(const ZipSink &) = delete;

    // False when the chunk would pass the declared size, or would not fit.
    bool take(Str chunk);

    // True only when exactly the declared size arrived.
    bool complete() const { return out_.size() == want_; }

    String &text() { return out_; }

private:
    u64 want_;
    String out_;
};

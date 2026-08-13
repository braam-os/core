// BundleFs — a read-only tree unpacked from one archive (Concept.md §5.1).
// Concept.md has it coming out of the Cache API; until M6 brings `fetch`, the
// worker loads it beside kernel.wasm at boot and hands the bytes over.
//
// The archive, as tools/pack.py writes it, little-endian throughout:
//
//   "BBND", u32 version, u32 count
//   count × { u32 name_off, u32 name_len, u32 data_off, u32 size }
//   names and data, in any order, at those offsets
//
// Names are paths relative to the mount point, with no leading slash.
// Directories are implicit: they exist because something beneath them does.
#pragma once

#include "fs.h"
#include "kernel/vec.h"

struct BundleFs final : Fs {
    Str kind() const override { return "bundle"; }

    bool writable() const override { return false; }

    Task<Result<Stat>> stat(Str path) override;
    Task<Result<Vec<Entry>>> list(Str path) override;
    Task<Result<u32>> open(Str path, u32 flags) override;
    Task<Result<void>> mkdir(Str path) override;
    Task<Result<void>> remove(Str path, bool all) override;

    Result<usize> read(u32 h, u64 off, u8 *buf, usize n) override;
    Result<usize> write(u32 h, u64 off, const u8 *buf, usize n) override;
    Result<u64> size(u32 h) override;
    Result<void> truncate(u32 h, u64 n) override;
    void close(u32 h) override;

    u64 bytes() const override;

private:
    friend Result<BundleFs *> bundlefs_create(String archive);

    struct File {
        Str name; // views archive_
        u32 off = 0, len = 0;
    };

    // The name a path names, or npos.
    usize find(Str path) const;

    String archive_;
    Vec<File> files_;
    Vec<i32> open_; // handle -> index into files_, or -1
};

// Takes the archive bytes and validates them. Err(Invalid) means the header
// did not check out, which is a packing bug rather than a missing bundle.
Result<BundleFs *> bundlefs_create(String archive);

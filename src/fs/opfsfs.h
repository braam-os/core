// OpfsFs — the Origin Private File System, and the only store that survives a
// reload (Concept.md §5.2). Every method is a call to the host: the naming
// operations are asynchronous, and the operations on an open file are not,
// because a sync access handle really is synchronous.
#pragma once

#include "fs.h"

struct OpfsFs final : Fs {
    Str kind() const override { return "opfs"; }

    bool writable() const override { return true; }

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
};

// Decodes what the host packs for a List reply. Shared with the tests, which
// is the only reason it is not private to opfsfs.cpp.
Result<Vec<Entry>> fs_decode_entries(const char *p, usize n);

// MemFs — a node tree in linear memory. It backs /tmp, the root, and /home
// when there is no OPFS to put it on (Concept.md §5.1).
//
// A tree of nodes rather than a HashMap keyed by path: String is move-only and
// HashMap::find takes its key by value, so a path-keyed map cannot be spelled
// with the containers we have — and a tree is what list() and rename want.
#pragma once

#include "fs.h"
#include "kernel/vec.h"

struct MemNode {
    String name;
    NodeKind kind = NodeKind::File;
    String data;         // files
    Vec<MemNode *> kids; // directories

    ~MemNode();
};

struct MemFs final : Fs {
    MemFs();

    ~MemFs() override;

    Str kind() const override { return "memfs"; }

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

    // Bytes held by every file in the tree, for `df` on a memory mount.
    u64 bytes() const override;

private:
    struct Open {
        MemNode *node = nullptr;
        bool used     = false;
    };

    MemNode *walk(Str path);
    MemNode *child(MemNode *dir, Str name);
    Result<MemNode *> make(MemNode *dir, Str name, NodeKind kind);
    MemNode *node_of(u32 h);

    MemNode root_;
    Vec<Open> open_;
};

// The local store (Package_Format.md §8): the paths under /pkg, §8.2's two
// text files, and a generation as a list of steps store.h performs.
//
// Syscall-free. Every Str views the text the caller holds.
#pragma once

#include "kernel/span.h"
#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/types.h"
#include "kernel/vec.h"

// --------------------------------------------------------------- §8, the tree

constexpr Str PKG_DIR    = "/pkg";
constexpr Str PKG_STORE  = "/pkg/store";
constexpr Str PKG_DB     = "/pkg/db";
constexpr Str PKG_GEN    = "/pkg/gen";
constexpr Str PKG_CACHE  = "/pkg/cache";
constexpr Str PKG_ACTIVE = "/pkg/active";
constexpr Str PKG_BIN    = "/pkg/bin";
constexpr Str PKG_WORLD  = "/pkg/world";
constexpr Str PKG_REPOS  = "/pkg/repositories";
constexpr Str PKG_INDEX  = "/pkg/index";

// Renamed over /pkg/active. That rename is the commit.
constexpr Str PKG_ACTIVE_NEW = "/pkg/active.new";

// <name>-<version>: a store directory, and a db file.
bool pkg_stem(Str name, Str version, String &out);

bool pkg_store_dir(Str name, Str version, Str leaf, String &out);
bool pkg_db_file(Str name, Str version, String &out);
bool pkg_gen_dir(u32 n, Str leaf, String &out);

// The generation a /pkg/active target names, or 0. Absolute or relative.
u32 gen_of(Str target);

// ------------------------------------------------------- §8.2, the two files

// One line of /pkg/gen/<N>/packages: two fields, positional, both required.
struct Installed {
    Str name;
    Str version;
};

// Sorted by name here, not by the caller: order is the writer's concern.
bool packages_write(Span<const Installed> v, String &out);
bool packages_read(Str text, Vec<Installed> &out);

// One §6 dependency per line.
bool world_write(Span<const Str> deps, String &out);
bool world_read(Str text, Vec<Str> &out);

// One URL per line; a blank line is skipped.
bool repos_read(Str text, Vec<Str> &out);

// ------------------------------------------------------------------ the steps

enum class StoreOpKind {
    MkDir,  // every missing component
    Write,  // a whole file, replacing what is there
    Link,   // a symbolic link, replacing what is there
    Rename, // `data` is the destination
    Remove, // recursively
};

struct StoreOp {
    StoreOpKind kind = StoreOpKind::MkDir;
    String path;
    String data; // Write: the bytes. Link: the target. Rename: the new name.
};

// The directories §8 names, and /pkg/bin. Idempotent; /pkg/bin may dangle.
bool pkg_tree_ops(Vec<StoreOp> &out);

// One entry of a generation's bin/.
struct GenLink {
    Str command;
    Str name;
    Str version;
};

// The directory, the text, the link farm, and the rename that commits them.
// Links are emitted in the order given; two naming one command leave the later.
bool gen_ops(u32 n, Span<const Installed> pkgs, Span<const GenLink> links, Vec<StoreOp> &out);

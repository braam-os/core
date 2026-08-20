// apk's dependency solver: greedy, deductive, no backtracking. Pure — the
// stanzas are the caller's, so a syscall here is a link error.
#pragma once

#include "dep.h"
#include "kernel/result.h"
#include "kernel/span.h"
#include "kernel/str.h"
#include "kernel/types.h"
#include "kernel/vec.h"
#include "stanza.h"

// What compare_providers branches on. Pinning's are absent with pinning.
constexpr u32 SOLVE_UPGRADE   = 1;
constexpr u32 SOLVE_AVAILABLE = 2;
constexpr u32 SOLVE_REINSTALL = 4;
constexpr u32 SOLVE_LATEST    = 8;
constexpr u32 SOLVE_INSTALLED = 16;
constexpr u32 SOLVE_REMOVE    = 32;

// A name the user gave, and the flags it puts on that name's providers.
struct SolveRequest {
    Str name;
    u32 flags   = 0;
    u32 inherit = 0; // the subset dependencies inherit
};

struct SolveInput {
    Span<const PackageStanza> repo;
    Span<const PackageStanza> installed;
    Span<const Dep> world;
    Span<const SolveRequest> named;
    u32 flags = 0; // the whole run's
};

// Null old_pkg installs, null new_pkg removes, both adjusts. The order of
// `changes` is the order to perform them in.
struct SolveChange {
    const PackageStanza *old_pkg = nullptr;
    const PackageStanza *new_pkg = nullptr;
    bool reinstall               = false;
};

// A contradiction, labelled the way apk's reporter labels one.
enum class SolveFailKind {
    Package, // a selected package that cannot be installed
    Virtual, // a name whose providers are all unversioned
    Missing, // a name nothing provides
};

struct SolveFail {
    SolveFailKind kind = SolveFailKind::Missing;
    Str name;
    Str version; // Package only
};

struct Changeset {
    Vec<SolveChange> changes;
    u32 installs = 0, removals = 0, adjusts = 0;
    Vec<SolveFail> errors;
};

// Err is NoMemory alone. A contradiction is `out.errors`, and a failed solve
// still fills `out.changes`.
Result<void> solve(const SolveInput &in, Changeset &out);

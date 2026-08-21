// §7's steps 1 to 7 of Package_Management.md, in one function and in order.
// The host is a parameter, so nothing here needs a syscall.
#pragma once

#include "host.h"
#include "kernel/result.h"
#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/task.h"
#include "kernel/types.h"
#include "kernel/vec.h"
#include "stanza.h"

// Well below SYS_STAGE_MAX, which Sys::Verify stages whole.
constexpr usize INDEX_MAX = 1u << 19;

// The most a package archive may be, whatever S says: it is held whole.
constexpr u64 PACKAGE_MAX = 4u << 20;

constexpr u32 INDEX_GRAMMAR = 1;

// The index is at <N>/index (Package_Format.md §3.3).
constexpr Str INDEX_LEAF = "/index";

// A repository URL without its trailing slashes: <N>/index would be //index.
Str repo_trim(Str url);

// §7's order, and the step a refusal stopped at.
enum class IndexStep {
    Clock,     // 1
    Anchor,    // 2
    Fetch,     // 3, and the cap
    Signature, // 4
    Header,    // §3.1's X and N
    Version,   // 5
    Expiry,    // 6
    Read,      // 7
    Package,   // 8, and the cap
    Digest,    // 9
};

Str index_step_name(IndexStep s);

// A checked index: the text it owns, §2's split of it, and the records.
struct CheckedIndex {
    String text;
    Str block;
    Str body;
    Vec<Signature> sigs;
    IndexHeader head;
    Vec<PackageStanza> packages;
    u64 now        = 0;
    bool unchanged = false; // G equal to the floor: nothing to do, not an error
};

// `repo` is a repositories line, without a trailing slash. On failure `step`
// names the step that refused.
Task<Result<void>> index_check(PkgHost &h, Str repo, CheckedIndex &out, IndexStep &step);

// The stored /pkg/index: §2's split, the signature block, §3.1's header and
// the packages. §7's checks are not repeated. Takes the text every record
// views.
Result<void> index_read(String text, CheckedIndex &out);

// A name the index does not list does not exist, and is not looked for
// anywhere else (§7 step 7).
const PackageStanza *index_find(const CheckedIndex &c, Str name);

// The same under P or under p: a name the index offers. `cmd:awk` is one.
const PackageStanza *index_provides(const CheckedIndex &c, Str name);

// §7 step 9: the size and the digest the index gave, against what arrived.
bool package_check(const PackageStanza &p, Str bytes);

// §7 step 8: <N>/<name>-<version>.zip (§3.3), capped at the exact size the
// index gave, then step 9. Nothing under /pkg is touched until this is Ok —
// the rule's only crossing point.
Task<Result<void>> index_fetch(PkgHost &h, const CheckedIndex &c, const PackageStanza &p,
                               String &out, IndexStep &step);

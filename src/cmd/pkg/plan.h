// What a changeset means: apk's `print_change` verbs, its reporter's labels,
// and the installed set left behind. Pure — the stanzas are the caller's.
#pragma once

#include "db.h"
#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/types.h"
#include "kernel/vec.h"
#include "solve.h"
#include "zip.h"

// The most a package may unpack to when its stanza carries no I.
constexpr u64 UNPACK_MAX = 16u << 20;

// apk's verb, or empty for a change that changes nothing — which is also the
// test for whether a change is work.
Str plan_verb(const SolveChange &c);

// One line per change, in the changeset's order. No verb prints nothing.
bool plan_changes(const Changeset &cs, String &out);

// The two scripts a change runs (§5.1), around the commit. A change that is no
// work runs neither, and so is Payload — the kind that is not a script.
struct PlanScripts {
    ZipMeta before = ZipMeta::Payload;
    ZipMeta after  = ZipMeta::Payload;

    // The store directory they come out of, and their argv: the new version,
    // and on an upgrade the old one after it.
    const PackageStanza *pkg = nullptr;
    Str old_version;
};

PlanScripts plan_scripts(const SolveChange &c);

// `lead`, then one indented label per contradiction.
bool plan_errors(const Changeset &cs, Str lead, String &out);

// The set a changeset leaves installed, sorted by name. The link farm is built
// in this order, and gen_ops leaves the later of two links naming one command.
bool plan_installed(const Changeset &cs, Vec<Installed> &out);

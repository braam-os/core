// A package no index vouches for (Package_Management.md §7.1): an archive named
// by a path or a URL, whose .PKGINFO is its stanza. Nothing here needs a
// syscall; the file, the fetch and the inflate are install.cpp's.
#pragma once

#include "index.h"
#include "kernel/result.h"
#include "kernel/span.h"
#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/types.h"
#include "stanza.h"
#include "zip.h"

// How a `pkg install` operand names what to install.
enum class Operand {
    Name, // a §6 token, resolved in the index
    File, // ends .zip
    Url,  // holds ://
};

Operand operand_kind(Str word);

// A sideload: the archive, the texts its stanza views, and the stanza. String's
// move takes the block, so the views survive the Vec holding these growing.
struct LocalPackage {
    String zip;      // the archive, whole
    String info;     // .PKGINFO as it came out of it
    String provides; // p:, with §6.1's cmd: names appended
    PackageStanza stanza;
};

// Where local_stanza refused.
enum class LocalStep {
    Read,     // the file would not read, or the fetch did not answer
    Archive,  // not a zip this reader reads
    Metadata, // no .PKGINFO, or a dot-entry nothing knows
    Pkginfo,  // it does not read, or P or V is not a path component
    Index,    // the index lists this version at another digest
};

Str local_step_name(LocalStep s);

// Builds `out.stanza` from `out.info`, `out.zip` and the entries: C is the
// archive's digest and S its size (§5.1's two letters that cannot be inside
// it), and §6.1's cmd: names come off the flat bin/. `zip` and `info` are the
// caller's to fill first.
Result<void> local_stanza(LocalPackage &out, Span<const ZipEntry> entries, LocalStep &step);

// A name-version the index lists at another digest: refused, so the index stays
// authoritative wherever it speaks. An equal digest is the same package.
bool local_conflicts(const CheckedIndex &c, const PackageStanza &p);

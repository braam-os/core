#include "local.h"

#include "sha256.h"

// Package_Management.md §7.1.

namespace {

constexpr Str ZIP_EXT = ".zip";
constexpr Str SCHEME  = "://";
constexpr Str CMD     = "cmd:";
constexpr Str BIN     = "bin/";

// §6.1: cmd:<leaf>=<V> per flat bin/ entry — what mkindex.py writes into an
// index stanza, and a .PKGINFO need carry none.
bool derive_commands(Span<const ZipEntry> entries, Str version, String &out)
{
    for (const ZipEntry &e : entries) {
        if (!e.name.starts_with(BIN))
            continue;
        Str leaf = e.name.substr(BIN.size());
        if (leaf.empty() || leaf.find('/') != Str::npos)
            continue;
        if (!out.empty() && !out.push(' '))
            return false;
        if (!out.append(CMD) || !out.append(leaf) || !out.push('=') || !out.append(version))
            return false;
    }
    return true;
}

} // namespace

Operand operand_kind(Str word)
{
    if (word.contains(SCHEME))
        return Operand::Url;
    if (word.size() > ZIP_EXT.size() && word.ends_with(ZIP_EXT))
        return Operand::File;
    return Operand::Name;
}

Str local_step_name(LocalStep s)
{
    switch (s) {
    case LocalStep::Read:
        return "read";
    case LocalStep::Archive:
        return "archive";
    case LocalStep::Metadata:
        return "metadata";
    case LocalStep::Pkginfo:
        return "pkginfo";
    case LocalStep::Index:
        return "index";
    }
    return "read";
}

Result<void> local_stanza(LocalPackage &out, Span<const ZipEntry> entries, LocalStep &step)
{
    // §5.1: an unknown top-level dot-entry makes the package uninstallable.
    step = LocalStep::Metadata;
    for (const ZipEntry &e : entries)
        if (zip_meta(e.name) == ZipMeta::Unknown)
            return Err(Error::Unsupported);

    step = LocalStep::Pkginfo;
    Vec<StanzaField> f;
    if (!StanzaReader::one(out.info.str(), STANZA_PACKAGE, f))
        return Err(Error::Invalid);

    // Refuses a C or an S, and holds P and V to a path component: the store
    // path is built out of those two and nothing vouched for them.
    PackageStanza p;
    if (package_read_info(f, p) != StanzaRead::Ok)
        return Err(Error::Invalid);

    Sha256 h;
    h.update(out.zip.str());
    h.finish(p.digest);
    p.size = out.zip.size();

    if (!out.provides.assign(p.provides) || !derive_commands(entries, p.version, out.provides))
        return Err(Error::NoMemory);
    p.provides = out.provides.str();

    out.stanza = p;
    return {};
}

bool local_conflicts(const CheckedIndex &c, const PackageStanza &p)
{
    for (const PackageStanza &st : c.packages) {
        if (!(st.name == p.name) || !(st.version == p.version))
            continue;
        for (usize i = 0; i < SHA256_SIZE; i++)
            if (st.digest[i] != p.digest[i])
                return true;
    }
    return false;
}

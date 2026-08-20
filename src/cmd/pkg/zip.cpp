#include "zip.h"

namespace {

constexpr u32 EOCD_SIG    = 0x06054b50;
constexpr u32 CENTRAL_SIG = 0x02014b50;
constexpr u32 LOCAL_SIG   = 0x04034b50;

constexpr usize EOCD_BYTES    = 22;
constexpr usize CENTRAL_BYTES = 46;
constexpr usize LOCAL_BYTES   = 30;

constexpr usize COMMENT_MAX = 0xffff;

u32 u16at(Str s, usize at)
{
    return u32(u8(s[at])) | (u32(u8(s[at + 1])) << 8);
}

u32 u32at(Str s, usize at)
{
    return u16at(s, at) | (u16at(s, at + 2) << 16);
}

constexpr bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// A name out of an archive is not a path until it has been looked at.
bool name_ok(Str name)
{
    if (name.empty() || name[0] == '/')
        return false;
    if (name.size() >= 2 && is_alpha(name[0]) && name[1] == ':')
        return false;
    for (char c : name)
        if (c == '\\')
            return false;

    Str rest = name;
    while (!rest.empty()) {
        Str part = rest.split('/', rest);
        if (part == "." || part == "..")
            return false;
    }
    return true;
}

// The last EOCD in the comment window, or npos.
usize find_eocd(Str archive)
{
    if (archive.size() < EOCD_BYTES)
        return Str::npos;
    usize last  = archive.size() - EOCD_BYTES;
    usize first = last > COMMENT_MAX ? last - COMMENT_MAX : 0;
    for (usize at = last + 1; at-- > first;)
        if (u32at(archive, at) == EOCD_SIG)
            return at;
    return Str::npos;
}

} // namespace

ZipRead zip_entries(Str archive, Vec<ZipEntry> &out)
{
    usize eocd = find_eocd(archive);
    if (eocd == Str::npos)
        return ZipRead::Malformed;

    u32 count   = u16at(archive, eocd + 10);
    u32 catalog = u32at(archive, eocd + 16);
    // Zip64 announces itself by saturating these.
    if (count == 0xffff || catalog == 0xffffffff)
        return ZipRead::Unsupported;

    usize at = catalog;
    for (u32 i = 0; i < count; i++) {
        if (at > archive.size() || archive.size() - at < CENTRAL_BYTES ||
            u32at(archive, at) != CENTRAL_SIG)
            return ZipRead::Malformed;

        u32 flags     = u16at(archive, at + 8);
        u32 method    = u16at(archive, at + 10);
        usize packed  = u32at(archive, at + 20);
        u64 size      = u32at(archive, at + 24);
        usize namelen = u16at(archive, at + 28);
        usize local   = u32at(archive, at + 42);
        if (archive.size() - at - CENTRAL_BYTES < namelen)
            return ZipRead::Malformed;
        Str name = archive.substr(at + CENTRAL_BYTES, namelen);
        at += CENTRAL_BYTES + namelen + u16at(archive, at + 30) + u16at(archive, at + 32);

        if (flags & 1)
            return ZipRead::Unsupported; // encrypted
        if (name.ends_with("/"))
            continue; // a directory entry; the paths imply it anyway
        if (!name_ok(name))
            return ZipRead::Malformed;

        // Re-read the local header: taking the central directory's offset for
        // the data is the classic way to get this wrong.
        if (local > archive.size() || archive.size() - local < LOCAL_BYTES ||
            u32at(archive, local) != LOCAL_SIG)
            return ZipRead::Malformed;
        usize from = local + LOCAL_BYTES + u16at(archive, local + 26) + u16at(archive, local + 28);
        if (from > archive.size() || archive.size() - from < packed)
            return ZipRead::Malformed;

        // Last, as parseZip judges it: the order the two readers refuse in is
        // part of what they agree on.
        if (method != ZIP_STORE && method != ZIP_DEFLATE)
            return ZipRead::Unsupported;

        ZipEntry e;
        e.name   = name;
        e.data   = archive.substr(from, packed);
        e.method = method;
        e.size   = size;
        if (!out.push(e))
            return ZipRead::NoMemory;
    }
    return ZipRead::Ok;
}

bool zip_stored(const ZipEntry &e, Str &out)
{
    if (e.method != ZIP_STORE || e.data.size() != e.size)
        return false;
    out = e.data;
    return true;
}

ZipMeta zip_meta(Str name)
{
    if (name.empty() || name[0] != '.' || name.contains("/"))
        return ZipMeta::Payload;

    struct Named {
        Str name;
        ZipMeta meta;
    };
    constexpr Named TABLE[] = {
        { ".PKGINFO", ZipMeta::PkgInfo },
        { ".pre-install", ZipMeta::PreInstall },
        { ".post-install", ZipMeta::PostInstall },
        { ".pre-deinstall", ZipMeta::PreDeinstall },
        { ".post-deinstall", ZipMeta::PostDeinstall },
        { ".pre-upgrade", ZipMeta::PreUpgrade },
        { ".post-upgrade", ZipMeta::PostUpgrade },
        { ".trigger", ZipMeta::Trigger },
    };
    for (const Named &n : TABLE)
        if (n.name == name)
            return n.meta;
    return ZipMeta::Unknown;
}

bool ZipSink::take(Str chunk)
{
    if (chunk.size() > want_ - out_.size())
        return false;
    return out_.append(chunk);
}

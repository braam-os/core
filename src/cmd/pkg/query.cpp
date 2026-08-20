#include "cmd/sh/match.h"
#include "db.h"
#include "index.h"
#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/traits.h"
#include "pkg.h"
#include "proc/io.h"
#include "proc/time.h"
#include "stanza.h"
#include "store.h"

// search, info and list: what /pkg/index and the active generation already
// hold, printed. Nothing here fetches, checks or writes.

namespace {

constexpr Str USAGE_SEARCH = "usage: pkg search <pattern>\n";
constexpr Str USAGE_INFO   = "usage: pkg info <package>\n";
constexpr Str USAGE_LIST   = "usage: pkg list\n";
constexpr Str NO_INDEX     = "pkg: no index; run pkg update\n";
constexpr Str NO_MEMORY    = "pkg: out of memory\n";

// `info`'s label column, and the gap between the columns of a listing.
constexpr usize W_LABEL = 14;
constexpr usize GAP     = 2;

Task<Result<void>> put(u32 fd, Str text)
{
    co_return co_await write_all(fd, text);
}

// A field, then blanks out to `w`. A row's last column is not padded.
bool pad_to(String &out, Str s, usize w)
{
    if (!out.append(s))
        return false;
    for (usize i = s.size(); i < w; i++)
        if (!out.push(' '))
            return false;
    return true;
}

// ------------------------------------------------------------ the two files

// /pkg/index as `pkg update` left it. 0 is a read index; anything else is the
// exit status, its refusal already printed.
Task<i32> load(CheckedIndex &c)
{
    Result<String> text = Err(Error::NoMemory);
    if (Task<Result<String>> t = store_slurp(PKG_INDEX))
        text = co_await t;
    if (text.is_err()) {
        if (text.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("pkg", PKG_INDEX, text.error()))
            co_await e;
        co_return 1;
    }
    if (text.value().empty()) {
        if (Task<Result<void>> t = put(SYS_STDERR, NO_INDEX))
            co_await t;
        co_return 1;
    }

    Result<void> r = index_read(move(text.value()), c);
    if (r.is_err()) {
        if (Task<void> e = errln("pkg", PKG_INDEX, r.error()))
            co_await e;
        co_return 1;
    }
    co_return 0;
}

// The active generation's packages file. An empty `path` is no generation.
Task<Result<void>> generation(String &path, String &text)
{
    Result<u32> gen = Err(Error::NoMemory);
    if (Task<Result<u32>> t = store_active())
        gen = co_await t;
    if (gen.is_err())
        co_return Err(gen.error());
    if (gen.value() == 0)
        co_return Result<void>();

    if (!pkg_gen_dir(gen.value(), "packages", path))
        co_return Err(Error::NoMemory);
    Result<String> got = Err(Error::NoMemory);
    if (Task<Result<String>> t = store_slurp(path.str()))
        got = co_await t;
    if (got.is_err())
        co_return Err(got.error());
    text = move(got.value());
    co_return Result<void>();
}

// The version the generation carries for `name`. A view into `text`.
Str installed_of(Str text, Str name)
{
    Vec<Installed> pkgs;
    if (!packages_read(text, pkgs))
        return Str();
    for (const Installed &p : pkgs)
        if (p.name == name)
            return p.version;
    return Str();
}

// ---------------------------------------------------------------- search

char fold(char c)
{
    return c >= 'A' && c <= 'Z' ? char(c + 32) : c;
}

// Str carries no case handling.
bool has_fold(Str hay, Str needle)
{
    if (needle.size() > hay.size())
        return false;
    for (usize i = 0; i + needle.size() <= hay.size(); i++) {
        usize j = 0;
        while (j < needle.size() && fold(hay[i + j]) == fold(needle[j]))
            j++;
        if (j == needle.size())
            return true;
    }
    return false;
}

// A live metacharacter globs the whole field, anything else is a substring
// that ignores case. An absent field matches nothing.
bool matches(Str pattern, bool glob, Str field)
{
    if (field.empty())
        return false;
    return glob ? glob_match(pattern, Span<const u8>(), field) : has_fold(field, pattern);
}

bool found(const PackageStanza &p, Str pattern, bool glob)
{
    return matches(pattern, glob, p.name) || matches(pattern, glob, p.description);
}

// ------------------------------------------------------------------ info

void put2(Buf<64> &b, u32 v)
{
    b.put(char('0' + (v / 10) % 10)).put(char('0' + v % 10));
}

bool field(String &out, Str name, Str value)
{
    if (value.empty())
        return true;
    return pad_to(out, name, W_LABEL) && out.append(value) && out.push('\n');
}

bool number(String &out, Str name, u64 v, bool always = false)
{
    if (v == 0 && !always)
        return true;
    Buf<32> b;
    b.put(v);
    return pad_to(out, name, W_LABEL) && out.append(b.str()) && out.push('\n');
}

// UTC: a build time is the publisher's, not the reader's.
bool built(String &out, u64 ms)
{
    if (ms == 0)
        return true;
    Civil c = civil(i64(ms / 1000));
    Buf<64> b;
    b.put(u32(c.year)).put('-');
    put2(b, c.month);
    b.put('-');
    put2(b, c.day);
    b.put(' ');
    put2(b, c.hour);
    b.put(':');
    put2(b, c.min);
    b.put(':');
    put2(b, c.sec);
    b.put(" UTC");
    return field(out, "built", b.str());
}

// §3.3's order, with C last.
bool describe(String &out, const PackageStanza &p, Str installed)
{
    char digest[DIGEST_TEXT];
    if (!digest_write(p.digest, Span<char>(digest)))
        return false;

    return field(out, "name", p.name) && field(out, "version", p.version) &&
           field(out, "installed", installed) && number(out, "size", p.size, true) &&
           number(out, "unpacked", p.installed_size) && field(out, "description", p.description) &&
           field(out, "origin", p.origin) && built(out, p.build_time) &&
           number(out, "priority", p.priority) && field(out, "triggers", p.globs) &&
           field(out, "depends", p.depends) && field(out, "provides", p.provides) &&
           field(out, "install-if", p.install_if) &&
           field(out, "digest", Str(digest, sizeof(digest)));
}

// ------------------------------------------------------------- the frame

// A CheckedIndex is too much for a coroutine frame.
struct Held {
    ~Held() { heap_delete(p); }
    CheckedIndex *p;
};

Task<Result<void>> emit(Str text)
{
    co_return co_await write_all(SYS_STDOUT, text);
}

} // namespace

Task<i32> pkg_search(Args args)
{
    if (args.size() != 2) {
        if (Task<Result<void>> t = put(SYS_STDERR, USAGE_SEARCH))
            co_await t;
        co_return 2;
    }

    Held held{ heap_new<CheckedIndex>() };
    if (!held.p) {
        if (Task<Result<void>> t = put(SYS_STDERR, NO_MEMORY))
            co_await t;
        co_return 1;
    }

    i32 bad = 1;
    if (Task<i32> t = load(*held.p))
        bad = co_await t;
    if (bad != 0)
        co_return bad;

    Str pattern = args[1];
    bool glob   = glob_meta(pattern, Span<const u8>());

    // The widths come off the matches, not off the index.
    usize w_name = 0, w_version = 0;
    for (const PackageStanza &p : held.p->packages)
        if (found(p, pattern, glob)) {
            w_name    = max(w_name, p.name.size());
            w_version = max(w_version, p.version.size());
        }

    String out;
    for (const PackageStanza &p : held.p->packages) {
        if (!found(p, pattern, glob))
            continue;
        bool ok = pad_to(out, p.name, w_name + GAP);
        if (p.description.empty())
            ok = ok && out.append(p.version);
        else
            ok = ok && pad_to(out, p.version, w_version + GAP) && out.append(p.description);
        if (!ok || !out.push('\n')) {
            if (Task<Result<void>> t = put(SYS_STDERR, NO_MEMORY))
                co_await t;
            co_return 1;
        }
    }

    // Nothing matched is nothing to print, and still a success.
    if (out.empty())
        co_return 0;
    Result<void> w = Err(Error::NoMemory);
    if (Task<Result<void>> t = emit(out.str()))
        w = co_await t;
    co_return w.is_ok() ? 0 : 1;
}

Task<i32> pkg_info(Args args)
{
    if (args.size() != 2) {
        if (Task<Result<void>> t = put(SYS_STDERR, USAGE_INFO))
            co_await t;
        co_return 2;
    }

    Held held{ heap_new<CheckedIndex>() };
    if (!held.p) {
        if (Task<Result<void>> t = put(SYS_STDERR, NO_MEMORY))
            co_await t;
        co_return 1;
    }

    i32 bad = 1;
    if (Task<i32> t = load(*held.p))
        bad = co_await t;
    if (bad != 0)
        co_return bad;

    // §7 step 7: a name the index does not list does not exist.
    const PackageStanza *p = index_find(*held.p, args[1]);
    if (!p) {
        String line;
        if (line.append("pkg: ") && line.append(args[1]) && line.append(": not in the index\n"))
            if (Task<Result<void>> t = put(SYS_STDERR, line.str()))
                co_await t;
        co_return 1;
    }

    String path, text;
    Result<void> g = Err(Error::NoMemory);
    if (Task<Result<void>> t = generation(path, text))
        g = co_await t;
    if (g.is_err()) {
        if (g.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("pkg", path.empty() ? PKG_ACTIVE : path.str(), g.error()))
            co_await e;
        co_return 1;
    }

    String out;
    if (!describe(out, *p, installed_of(text.str(), p->name))) {
        if (Task<Result<void>> t = put(SYS_STDERR, NO_MEMORY))
            co_await t;
        co_return 1;
    }

    Result<void> w = Err(Error::NoMemory);
    if (Task<Result<void>> t = emit(out.str()))
        w = co_await t;
    co_return w.is_ok() ? 0 : 1;
}

Task<i32> pkg_list(Args args)
{
    if (args.size() != 1) {
        if (Task<Result<void>> t = put(SYS_STDERR, USAGE_LIST))
            co_await t;
        co_return 2;
    }

    String path, text;
    Result<void> g = Err(Error::NoMemory);
    if (Task<Result<void>> t = generation(path, text))
        g = co_await t;
    if (g.is_err()) {
        if (g.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("pkg", path.empty() ? PKG_ACTIVE : path.str(), g.error()))
            co_await e;
        co_return 1;
    }
    if (path.empty())
        co_return 0;

    Vec<Installed> pkgs;
    if (!packages_read(text.str(), pkgs)) {
        if (Task<void> e = errln("pkg", path.str(), Error::Invalid))
            co_await e;
        co_return 1;
    }

    // The writer sorted the file by name.
    usize w_name = 0;
    for (const Installed &p : pkgs)
        w_name = max(w_name, p.name.size());

    String out;
    for (const Installed &p : pkgs)
        if (!pad_to(out, p.name, w_name + GAP) || !out.append(p.version) || !out.push('\n')) {
            if (Task<Result<void>> t = put(SYS_STDERR, NO_MEMORY))
                co_await t;
            co_return 1;
        }

    if (out.empty())
        co_return 0;
    Result<void> w = Err(Error::NoMemory);
    if (Task<Result<void>> t = emit(out.str()))
        w = co_await t;
    co_return w.is_ok() ? 0 : 1;
}

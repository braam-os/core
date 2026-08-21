#include "db.h"
#include "kernel/alloc.h"
#include "kernel/traits.h"
#include "pkg.h"
#include "proc/io.h"
#include "sha256.h"
#include "stanza.h"
#include "store.h"

// files and verify: §8.1's record read back, and the bytes behind it hashed
// again. What that does not mean is Package_Management.md §11.

namespace {

constexpr Str USAGE_FILES   = "usage: pkg files <package>\n";
constexpr Str USAGE_VERIFY  = "usage: pkg verify [<package>]\n";
constexpr Str NOT_INSTALLED = "not installed";

// The report's first column.
constexpr Str MISSING  = "missing";
constexpr Str MODIFIED = "modified";
constexpr Str EXTRA    = "extra";
constexpr Str BROKEN   = "broken";
constexpr usize W_WORD = 10;

// Sha256 is 112 bytes, and a frame past 512 costs a whole span.
struct Job {
    String gen_path, gen_text;
    Vec<Installed> rows;

    String db_text;       // one record at a time; a DbRecord views it
    Vec<String> recorded; // what it names, relative to the store directory
    Vec<String> pending;  // the walk's directories, likewise

    String out;
    Sha256 hash;
};

struct Held {
    ~Held() { heap_delete(p); }
    Job *p;
};

Task<Result<void>> put(u32 fd, Str text)
{
    co_return co_await write_all(fd, text);
}

Task<void> complain(Str name, Str tail)
{
    String line;
    if (!line.assign("pkg: ") || !line.append(name) || !line.append(": ") || !line.append(tail) ||
        !line.push('\n'))
        co_return;
    if (Task<Result<void>> t = put(SYS_STDERR, line.str()))
        co_await t;
}

// The active generation and its lines. Nonzero is the exit status, refused.
Task<i32> read_generation(Job &in)
{
    Result<void> g = Err(Error::NoMemory);
    if (Task<Result<void>> t = pkg_generation(in.gen_path, in.gen_text))
        g = co_await t;
    if (g.is_err()) {
        if (g.error() == Error::Cancelled)
            co_return 130;
        Str what = in.gen_path.empty() ? PKG_ACTIVE : in.gen_path.str();
        if (Task<void> e = errln("pkg", what, g.error()))
            co_await e;
        co_return 1;
    }

    // No generation is nothing installed, and not an error.
    if (in.gen_path.empty())
        co_return 0;
    if (!packages_read(in.gen_text.str(), in.rows)) {
        if (Task<void> e = errln("pkg", in.gen_path.str(), Error::Invalid))
            co_await e;
        co_return 1;
    }
    co_return 0;
}

// /pkg/db/<stem>, parsed. The text stays in `in`, since the record views it.
Task<Result<void>> read_record(Job &in, Str name, Str version, DbRecord &rec)
{
    Result<String> got = Err(Error::NoMemory);
    if (Task<Result<String>> t = store_db_get(name, version))
        got = co_await t;
    if (got.is_err())
        co_return Err(got.error());
    in.db_text = move(got.value());

    Vec<StanzaField> f;
    if (!StanzaReader::one(in.db_text.str(), STANZA_DB, f) || db_read(f, rec) != StanzaRead::Ok)
        co_return Err(Error::Invalid);
    co_return Result<void>();
}

Task<void> refuse_record(Str name, Str version, Error why)
{
    String path;
    if (!pkg_db_file(name, version, path))
        co_return;
    if (Task<void> e = errln("pkg", path.str(), why))
        co_await e;
}

bool report(String &out, Str word, Str path)
{
    if (!out.append(word))
        return false;
    for (usize i = word.size(); i < W_WORD; i++)
        if (!out.push(' '))
            return false;
    return out.append(path) && out.push('\n');
}

bool held(const Vec<String> &v, Str s)
{
    for (const String &had : v)
        if (had.str() == s)
            return true;
    return false;
}

// One file, hashed off the store as it is read. Ok(true) is the digest the
// record holds, Ok(false) another, Err(NotFound) nothing there.
Task<Result<bool>> file_matches(Sha256 &h, Str path, const u8 want[SHA256_SIZE])
{
    Task<Result<i32>> o = open_read(path);
    if (!o)
        co_return Err(Error::NoMemory);
    Result<i32> fd = co_await o;
    if (fd.is_err())
        co_return Err(fd.error());

    h.reset();
    Result<void> bad = {};
    for (;;) {
        Task<Result<String>> t = read_chunk(u32(fd.value()));
        if (!t) {
            bad = Err(Error::NoMemory);
            break;
        }
        Result<String> r = co_await t;
        if (r.is_err()) {
            if (r.error() != Error::Closed)
                bad = Err(r.error());
            break;
        }
        h.update(r.value().str());
    }
    if (Task<void> c = close_fd(u32(fd.value())))
        co_await c;
    if (bad.is_err())
        co_return Err(bad.error());

    u8 got[SHA256_SIZE];
    h.finish(got);
    for (usize i = 0; i < SHA256_SIZE; i++)
        if (got[i] != want[i])
            co_return false;
    co_return true;
}

// What the store holds under `name`-`version` that `in.recorded` does not.
// Iterative: a coroutine per level would be a frame per level.
Task<Result<void>> extras(Job &in, Str name, Str version)
{
    in.pending.clear();
    String root;
    if (!in.pending.push(move(root)))
        co_return Err(Error::NoMemory);

    for (usize at = 0; at < in.pending.size(); at++) {
        String dir;
        if (!dir.assign(in.pending[at].str()))
            co_return Err(Error::NoMemory);

        String path;
        if (!pkg_store_dir(name, version, dir.str(), path))
            co_return Err(Error::NoMemory);

        Result<Vec<DirEntry>> got = Err(Error::NoMemory);
        if (Task<Result<Vec<DirEntry>>> t = list_dir(path.str()))
            got = co_await t;
        if (got.is_err()) {
            if (got.error() == Error::NotFound || got.error() == Error::NotDir)
                continue;
            co_return Err(got.error());
        }

        for (const DirEntry &e : got.value()) {
            String rel;
            if (!db_join(dir.str(), e.name.str(), rel))
                co_return Err(Error::NoMemory);
            if (e.kind == SYS_KIND_DIR) {
                if (!in.pending.push(move(rel)))
                    co_return Err(Error::NoMemory);
                continue;
            }
            if (held(in.recorded, rel.str()))
                continue;
            String abs;
            if (!pkg_store_dir(name, version, rel.str(), abs) || !report(in.out, EXTRA, abs.str()))
                co_return Err(Error::NoMemory);
        }
    }
    co_return Result<void>();
}

// 0 clean, 1 a line reported or a refusal printed, 130 cancelled.
Task<i32> verify_one(Job &in, Str name, Str version)
{
    DbRecord rec;
    Result<void> got = Err(Error::NoMemory);
    if (Task<Result<void>> t = read_record(in, name, version, rec))
        got = co_await t;
    if (got.is_err()) {
        if (got.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = refuse_record(name, version, got.error()))
            co_await e;
        co_return 1;
    }

    in.recorded.clear();
    i32 bad = 0;

    // §11: a script that failed is recorded, not fatal, and this is what finds
    // it. The path is the record that says so.
    if (!rec.broken.empty()) {
        String path;
        if (!pkg_db_file(name, version, path) || !report(in.out, BROKEN, path.str()))
            co_return 1;
        bad = 1;
    }

    for (const DbFile &f : rec.files) {
        String rel;
        if (!db_join(f.dir, f.name, rel))
            co_return 1;
        String path;
        if (!pkg_store_dir(name, version, rel.str(), path))
            co_return 1;
        if (!in.recorded.push(move(rel)))
            co_return 1;

        Result<bool> same = Err(Error::NoMemory);
        if (Task<Result<bool>> t = file_matches(in.hash, path.str(), f.digest))
            same = co_await t;
        if (same.is_err()) {
            if (same.error() == Error::Cancelled)
                co_return 130;
            if (same.error() != Error::NotFound) {
                if (Task<void> e = errln("pkg", path.str(), same.error()))
                    co_await e;
                bad = 1;
                continue;
            }
        }
        if (same.is_ok() && same.value())
            continue;
        if (!report(in.out, same.is_err() ? MISSING : MODIFIED, path.str()))
            co_return 1;
        bad = 1;
    }

    Result<void> walked = Err(Error::NoMemory);
    if (Task<Result<void>> t = extras(in, name, version))
        walked = co_await t;
    if (walked.is_err()) {
        if (walked.error() == Error::Cancelled)
            co_return 130;
        String path;
        if (!pkg_store_dir(name, version, "", path))
            co_return 1;
        if (Task<void> e = errln("pkg", path.str(), walked.error()))
            co_await e;
        co_return 1;
    }
    co_return bad;
}

} // namespace

Task<i32> pkg_files(Args args)
{
    if (args.size() != 2) {
        if (Task<Result<void>> t = put(SYS_STDERR, USAGE_FILES))
            co_await t;
        co_return 2;
    }

    Held h{ heap_new<Job>() };
    if (!h.p)
        co_return 1;
    Job &in = *h.p;

    i32 bad = 1;
    if (Task<i32> t = read_generation(in))
        bad = co_await t;
    if (bad != 0)
        co_return bad;

    Str name    = args[1];
    Str version = installed_version(in.gen_text.str(), name);
    if (version.empty()) {
        if (Task<void> e = complain(name, NOT_INSTALLED))
            co_await e;
        co_return 1;
    }

    DbRecord rec;
    Result<void> got = Err(Error::NoMemory);
    if (Task<Result<void>> t = read_record(in, name, version, rec))
        got = co_await t;
    if (got.is_err()) {
        if (got.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = refuse_record(name, version, got.error()))
            co_await e;
        co_return 1;
    }

    for (const DbFile &f : rec.files) {
        String rel, path;
        if (!db_join(f.dir, f.name, rel) || !pkg_store_dir(name, version, rel.str(), path))
            co_return 1;
        if (!in.out.append(path.str()) || !in.out.push('\n'))
            co_return 1;
    }

    if (in.out.empty())
        co_return 0;
    Result<void> w = Err(Error::NoMemory);
    if (Task<Result<void>> t = put(SYS_STDOUT, in.out.str()))
        w = co_await t;
    co_return w.is_ok() ? 0 : 1;
}

Task<i32> pkg_verify(Args args)
{
    if (args.size() > 2) {
        if (Task<Result<void>> t = put(SYS_STDERR, USAGE_VERIFY))
            co_await t;
        co_return 2;
    }

    Held h{ heap_new<Job>() };
    if (!h.p)
        co_return 1;
    Job &in = *h.p;

    i32 bad = 1;
    if (Task<i32> t = read_generation(in))
        bad = co_await t;
    if (bad != 0)
        co_return bad;

    // One operand, or the whole generation in the order it was written.
    if (args.size() == 2) {
        Str name    = args[1];
        Str version = installed_version(in.gen_text.str(), name);
        if (version.empty()) {
            if (Task<void> e = complain(name, NOT_INSTALLED))
                co_await e;
            co_return 1;
        }
        in.rows.clear();
        if (!in.rows.push(Installed{ name, version }))
            co_return 1;
    }

    bad = 0;
    for (const Installed &p : in.rows) {
        i32 one = 1;
        if (Task<i32> t = verify_one(in, p.name, p.version))
            one = co_await t;
        if (one == 130)
            co_return 130;
        if (one != 0)
            bad = 1;
    }

    if (!in.out.empty()) {
        Result<void> w = Err(Error::NoMemory);
        if (Task<Result<void>> t = put(SYS_STDOUT, in.out.str()))
            w = co_await t;
        if (w.is_err())
            co_return 1;
    }
    co_return bad;
}

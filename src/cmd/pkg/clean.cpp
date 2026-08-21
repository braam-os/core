#include "db.h"
#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/traits.h"
#include "pkg.h"
#include "proc/io.h"
#include "store.h"

// clean: the download cache, the store directories no kept generation names,
// and the generations below the one a rollback swings back to.

namespace {

constexpr Str USAGE   = "usage: pkg clean\n";
constexpr Str NOTHING = "nothing to clean\n";

struct Job {
    Vec<u32> keep, drop;
    Vec<String> texts; // the kept generations' packages; Installed views them
    Vec<Installed> wanted;
    Vec<String> stems; // what /pkg/store and /pkg/db hold, together
    Vec<StoreOp> ops;
    String out;
    usize archives = 0;
};

struct Held {
    ~Held() { heap_delete(p); }
    Job *p;
};

Task<Result<void>> put(u32 fd, Str text)
{
    co_return co_await write_all(fd, text);
}

bool has(const Vec<String> &v, Str s)
{
    for (const String &had : v)
        if (had.str() == s)
            return true;
    return false;
}

bool drop_op(Vec<StoreOp> &out, Str path)
{
    StoreOp op;
    op.kind = StoreOpKind::Remove;
    return op.path.assign(path) && out.push(move(op));
}

// A listing, or nothing where the directory is not there.
Task<Result<Vec<DirEntry>>> entries(Str path)
{
    Result<Vec<DirEntry>> got = Err(Error::NoMemory);
    if (Task<Result<Vec<DirEntry>>> t = list_dir(path))
        got = co_await t;
    if (got.is_err() && (got.error() == Error::NotFound || got.error() == Error::NotDir))
        co_return Vec<DirEntry>();
    co_return got;
}

// What the kept generations name. A generation whose text will not read stops
// the clean: it is the only record of what its store directories are for.
Task<Result<void>> wanted_of(Job &in)
{
    for (u32 n : in.keep) {
        String path;
        if (!pkg_gen_dir(n, "packages", path))
            co_return Err(Error::NoMemory);

        Result<String> got = Err(Error::NoMemory);
        if (Task<Result<String>> t = read_file(path.str()))
            got = co_await t;
        if (got.is_err()) {
            if (Task<void> e = errln("pkg", path.str(), got.error()))
                co_await e;
            co_return Err(got.error());
        }
        if (!in.texts.push(move(got.value())))
            co_return Err(Error::NoMemory);
        if (!packages_read(in.texts.back().str(), in.wanted)) {
            if (Task<void> e = errln("pkg", path.str(), Error::Invalid))
                co_await e;
            co_return Err(Error::Invalid);
        }
    }
    co_return Result<void>();
}

// Every stem under /pkg/store or /pkg/db that no kept generation names.
Task<Result<void>> stems_of(Job &in)
{
    Vec<String> want;
    for (const Installed &p : in.wanted) {
        String stem;
        if (!pkg_stem(p.name, p.version, stem) || !want.push(move(stem)))
            co_return Err(Error::NoMemory);
    }

    constexpr Str DIRS[] = { PKG_STORE, PKG_DB };
    for (Str dir : DIRS) {
        Result<Vec<DirEntry>> got = Err(Error::NoMemory);
        if (Task<Result<Vec<DirEntry>>> t = entries(dir))
            got = co_await t;
        if (got.is_err()) {
            if (Task<void> e = errln("pkg", dir, got.error()))
                co_await e;
            co_return Err(got.error());
        }
        for (DirEntry &e : got.value()) {
            if (has(want, e.name.str()) || has(in.stems, e.name.str()))
                continue;
            if (!in.stems.push(move(e.name)))
                co_return Err(Error::NoMemory);
        }
    }
    co_return Result<void>();
}

// The cache, whole: an archive is re-hashed every time it is used, so losing
// one costs a download (Package_Format.md §8).
Task<Result<void>> cache_of(Job &in)
{
    Result<Vec<DirEntry>> got = Err(Error::NoMemory);
    if (Task<Result<Vec<DirEntry>>> t = entries(PKG_CACHE))
        got = co_await t;
    if (got.is_err()) {
        if (Task<void> e = errln("pkg", PKG_CACHE, got.error()))
            co_await e;
        co_return Err(got.error());
    }

    for (const DirEntry &e : got.value()) {
        String path;
        if (!path.assign(PKG_CACHE) || !path.push('/') || !path.append(e.name.str()))
            co_return Err(Error::NoMemory);
        if (!drop_op(in.ops, path.str()))
            co_return Err(Error::NoMemory);
        in.archives++;
    }
    co_return Result<void>();
}

bool count(String &out, usize n, Str one, Str many)
{
    Buf<32> b;
    b.put(u64(n));
    return out.append(b.str()) && out.push(' ') && out.append(n == 1 ? one : many);
}

// "Dropping hello (1.0-r0)", or the stem itself where nothing built the name.
bool dropping(String &out, Str stem)
{
    Str name, version;
    if (!out.append("Dropping "))
        return false;
    if (!pkg_stem_split(stem, name, version))
        return out.append(stem) && out.push('\n');
    return out.append(name) && out.append(" (") && out.append(version) && out.append(")\n");
}

} // namespace

Task<i32> pkg_clean(Args args)
{
    if (args.size() != 1) {
        if (Task<Result<void>> t = put(SYS_STDERR, USAGE))
            co_await t;
        co_return 2;
    }

    Held h{ heap_new<Job>() };
    if (!h.p)
        co_return 1;
    Job &in = *h.p;

    Result<u32> active = Err(Error::NoMemory);
    if (Task<Result<u32>> t = store_active())
        active = co_await t;
    if (active.is_err()) {
        if (active.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("pkg", PKG_ACTIVE, active.error()))
            co_await e;
        co_return 1;
    }

    Result<Vec<u32>> gens = Err(Error::NoMemory);
    if (Task<Result<Vec<u32>>> t = store_generations())
        gens = co_await t;
    if (gens.is_err()) {
        if (gens.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("pkg", PKG_GEN, gens.error()))
            co_await e;
        co_return 1;
    }

    if (!gen_keep(gens.value(), active.value(), in.keep))
        co_return 1;
    for (u32 n : gens.value()) {
        bool kept = false;
        for (u32 k : in.keep)
            kept = kept || k == n;
        if (!kept && !in.drop.push(n))
            co_return 1;
    }

    Result<void> r = Err(Error::NoMemory);
    if (Task<Result<void>> t = wanted_of(in))
        r = co_await t;
    if (r.is_ok())
        if (Task<Result<void>> t = stems_of(in))
            r = co_await t;
    if (r.is_ok())
        if (Task<Result<void>> t = cache_of(in))
            r = co_await t;
    if (r.is_err())
        co_return r.error() == Error::Cancelled ? 130 : 1;

    // Nothing is removed until the whole list stands; each entry is already
    // unreferenced, so a list that stops part-way needs no unwinding.
    for (const String &stem : in.stems) {
        String path;
        if (!path.assign(PKG_STORE) || !path.push('/') || !path.append(stem.str()) ||
            !drop_op(in.ops, path.str()))
            co_return 1;
        if (!path.assign(PKG_DB) || !path.push('/') || !path.append(stem.str()) ||
            !drop_op(in.ops, path.str()))
            co_return 1;
        if (!dropping(in.out, stem.str()))
            co_return 1;
    }
    for (u32 n : in.drop) {
        String path;
        Buf<32> line;
        if (!pkg_gen_dir(n, "", path) || !drop_op(in.ops, path.str()))
            co_return 1;
        line.put("Dropping generation ").put(n).put('\n');
        if (!in.out.append(line.str()))
            co_return 1;
    }

    if (in.ops.empty()) {
        if (Task<Result<void>> t = put(SYS_STDOUT, NOTHING))
            co_await t;
        co_return 0;
    }

    Result<void> done = Err(Error::NoMemory);
    if (Task<Result<void>> t = store_perform(in.ops))
        done = co_await t;
    if (done.is_err()) {
        if (done.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("pkg", PKG_DIR, done.error()))
            co_await e;
        co_return 1;
    }

    if (!count(in.out, in.archives, "archive, ", "archives, ") ||
        !count(in.out, in.stems.size(), "package, ", "packages, ") ||
        !count(in.out, in.drop.size(), "generation\n", "generations\n"))
        co_return 1;

    Result<void> w = Err(Error::NoMemory);
    if (Task<Result<void>> t = put(SYS_STDOUT, in.out.str()))
        w = co_await t;
    co_return w.is_ok() ? 0 : 1;
}

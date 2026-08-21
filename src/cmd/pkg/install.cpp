#include "db.h"
#include "index.h"
#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/traits.h"
#include "pkg.h"
#include "plan.h"
#include "proc/io.h"
#include "sha256.h"
#include "solve.h"
#include "stanza.h"
#include "store.h"
#include "unzip.h"
#include "zip.h"

// The four commands that change the installed set. `install` is §7's steps 8
// to 10, the crossing between 9 and 10 being one line in `realise`; `remove`
// and `autoremove` are the same transaction with nothing to fetch, and
// `upgrade` is it once more with the solver told to prefer what is new.

namespace {

constexpr Str USAGE          = "usage: pkg install <package>...\n";
constexpr Str USAGE_REMOVE   = "usage: pkg remove <package>...\n";
constexpr Str USAGE_AUTO     = "usage: pkg autoremove\n";
constexpr Str USAGE_UPGRADE  = "usage: pkg upgrade\n";
constexpr Str NO_MEMORY      = "pkg: out of memory\n";
constexpr Str NOTHING        = "nothing installed\n";
constexpr Str CANNOT_INSTALL = "pkg: cannot install:";
constexpr Str CANNOT_REMOVE  = "pkg: cannot remove:";
constexpr Str CACHE_EXT      = ".zip";

// Everything one run holds: a frame past 512 bytes costs a 64 KiB span.
struct Txn {
    CheckedIndex index;

    String world_text; // /pkg/world as it was
    Vec<Str> specs;    // its lines, the operands merged in
    Vec<Dep> world;
    Vec<SolveRequest> named;
    u32 flags          = 0; // the run's own, which is SOLVE_UPGRADE or none
    bool world_changed = false;

    Vec<String> db_texts;         // the records the generation names
    Vec<PackageStanza> installed; // read out of them — C is the identity

    Changeset cset;
    Vec<Installed> want;

    Vec<String> record_paths, records; // db texts, serialised as each unpacks
    Vec<Installed> fresh;              // store dirs this run built
    Vec<String> commands;              // owned; GenLink views these
    Vec<GenLink> links;
    Vec<StoreOp> ops;

    String archive; // one package at a time
    Sha256 hash;
};

struct Held {
    ~Held() { heap_delete(p); }
    Txn *p;
};

Task<Result<void>> put(u32 fd, Str text)
{
    co_return co_await write_all(fd, text);
}

// "pkg: <what>: <why>", the step named between the colons.
Task<void> refuse(Str stem, Str step, Error why)
{
    String what;
    if (!what.assign(stem) || !what.append(": ") || !what.append(step))
        co_return;
    if (Task<void> e = errln("pkg", what.str(), why))
        co_await e;
}

Task<void> refuse_line(Str stem, Str tail)
{
    String line;
    if (!line.assign("pkg: ") || !line.append(stem) || !line.append(": ") || !line.append(tail) ||
        !line.push('\n'))
        co_return;
    if (Task<Result<void>> t = put(SYS_STDERR, line.str()))
        co_await t;
}

// ------------------------------------------------------------- the two reads

// The active generation as stanzas. solve() keys identity on the digest, so
// these must come off /pkg/db and cannot be built out of name and version.
Task<i32> read_installed(Txn &in)
{
    String path, text;
    Result<void> g = Err(Error::NoMemory);
    if (Task<Result<void>> t = pkg_generation(path, text))
        g = co_await t;
    if (g.is_err()) {
        if (Task<void> e = errln("pkg", path.empty() ? PKG_ACTIVE : path.str(), g.error()))
            co_await e;
        co_return 1;
    }
    if (path.empty())
        co_return 0;

    Vec<Installed> rows;
    if (!packages_read(text.str(), rows)) {
        if (Task<void> e = errln("pkg", path.str(), Error::Invalid))
            co_await e;
        co_return 1;
    }

    for (const Installed &row : rows) {
        String file;
        if (!pkg_db_file(row.name, row.version, file))
            co_return 1;

        Result<String> got = Err(Error::NoMemory);
        if (Task<Result<String>> t = store_db_get(row.name, row.version))
            got = co_await t;
        if (got.is_err()) {
            if (Task<void> e = errln("pkg", file.str(), got.error()))
                co_await e;
            co_return 1;
        }
        if (!in.db_texts.push(move(got.value())))
            co_return 1;

        Vec<StanzaField> f;
        DbRecord rec;
        bool ok = StanzaReader::one(in.db_texts.back().str(), STANZA_DB, f) &&
                  db_read(f, rec) == StanzaRead::Ok && rec.pkg.name == row.name &&
                  rec.pkg.version == row.version;
        if (!ok) {
            if (Task<void> e = errln("pkg", file.str(), Error::Invalid))
                co_await e;
            co_return 1;
        }
        if (!in.installed.push(move(rec.pkg)))
            co_return 1;
    }
    co_return 0;
}

// One past the highest /pkg/gen/<n>, not one past the active one: a rollback
// leaves higher generations standing and P22 keeps them.
Task<Result<u32>> next_gen()
{
    Result<Vec<DirEntry>> got = Err(Error::NoMemory);
    if (Task<Result<Vec<DirEntry>>> t = list_dir(PKG_GEN))
        got = co_await t;
    if (got.is_err()) {
        if (got.error() == Error::NotFound || got.error() == Error::NotDir)
            co_return u32(1);
        co_return Err(got.error());
    }

    u32 top = 0;
    for (const DirEntry &e : got.value()) {
        u32 n = gen_number(e.name.str());
        if (n > top)
            top = n;
    }
    co_return top + 1;
}

// ---------------------------------------------------------------- one package

// What /pkg already holds for a stem. The record is written after the unpack,
// so no record means no committed generation names it.
enum class Have {
    Missing, // nothing usable: fetch, rebuild, unpack
    Same,    // the digest the index gives now: nothing to do
    Other,   // the same name-version at another digest: refuse
};

Task<Result<Have>> stem_state(const PackageStanza &p)
{
    Result<String> got = Err(Error::NoMemory);
    if (Task<Result<String>> t = store_db_get(p.name, p.version))
        got = co_await t;
    if (got.is_err()) {
        if (got.error() == Error::NotFound)
            co_return Have::Missing;
        co_return Err(got.error());
    }

    Vec<StanzaField> f;
    DbRecord rec;
    if (!StanzaReader::one(got.value().str(), STANZA_DB, f) || db_read(f, rec) != StanzaRead::Ok)
        co_return Have::Missing;

    for (usize i = 0; i < SHA256_SIZE; i++)
        if (rec.pkg.digest[i] != p.digest[i])
            co_return Have::Other;

    String dir;
    if (!pkg_store_dir(p.name, p.version, "", dir))
        co_return Err(Error::NoMemory);
    Result<FileInfo> st = Err(Error::NoMemory);
    if (Task<Result<FileInfo>> t = stat_of(dir.str()))
        st = co_await t;
    if (st.is_err() || st.value().kind != SYS_KIND_DIR)
        co_return Have::Missing;
    co_return Have::Same;
}

bool cache_path(Str stem, String &out)
{
    return out.assign(PKG_CACHE) && out.push('/') && out.append(stem) && out.append(CACHE_EXT);
}

// The archive, from the cache when it hashes to what the index says and off
// the network otherwise. Either way §7 step 9 has passed when this is Ok.
Task<Result<void>> acquire(Txn &in, const PackageStanza &p, Str stem, IndexStep &step)
{
    String cache;
    if (!cache_path(stem, cache))
        co_return Err(Error::NoMemory);

    step                = IndexStep::Package;
    Result<FileInfo> st = Err(Error::NoMemory);
    if (Task<Result<FileInfo>> t = stat_of(cache.str()))
        st = co_await t;
    if (st.is_ok() && st.value().kind == SYS_KIND_FILE && st.value().size == p.size) {
        Result<String> got = Err(Error::NoMemory);
        if (Task<Result<String>> t = read_file(cache.str()))
            got = co_await t;
        step = IndexStep::Digest;
        if (got.is_ok() && package_check(p, got.value().str())) {
            in.archive = move(got.value());
            co_return Result<void>();
        }
    }

    if (Task<Result<void>> t = index_fetch(pkg_host(), in.index, p, in.archive, step))
        CO_TRY_VOID(co_await t);
    else
        co_return Err(Error::NoMemory);

    // A cache that will not write is not a failed install.
    if (Task<Result<void>> t = store_write(cache.str(), in.archive.str()))
        (void)co_await t;
    co_return Result<void>();
}

// §5.1's dot-entries: every one must be known, .PKGINFO must be there, and it
// must agree with the stanza that vouched for the package.
Result<void> check_meta(Span<const ZipEntry> entries, const PackageStanza &p, Str info)
{
    for (const ZipEntry &e : entries)
        if (zip_meta(e.name) == ZipMeta::Unknown)
            return Err(Error::Unsupported);

    // Read by field: C and S name the archive and cannot be inside it, so
    // .PKGINFO is not a whole §3.2 stanza and package_read would refuse it.
    Vec<StanzaField> f;
    if (!StanzaReader::one(info, STANZA_PACKAGE, f))
        return Err(Error::Invalid);

    Str name, version;
    for (const StanzaField &x : f) {
        if (x.letter == 'P')
            name = x.value;
        else if (x.letter == 'V')
            version = x.value;
    }

    // .PKGINFO authorises nothing; P and V are what choose the store path.
    if (name != p.name || version != p.version)
        return Err(Error::Perm);
    return {};
}

// I is inside the signed body and is therefore as trusted as C. Without one,
// UNPACK_MAX alone, which also bounds a single entry.
bool check_size(Span<const ZipEntry> entries, const PackageStanza &p)
{
    u64 cap = p.installed_size && p.installed_size < UNPACK_MAX ? p.installed_size : UNPACK_MAX;
    u64 sum = 0;
    for (const ZipEntry &e : entries) {
        if (zip_meta(e.name) != ZipMeta::Payload)
            continue;
        if (e.size > cap || sum > cap - e.size)
            return false;
        sum += e.size;
    }
    return true;
}

// The payload into /pkg/store/<stem>/, an entry at a time so that only one
// inflated file is held, and the record serialised before the archive goes.
Task<Result<void>> unpack(Txn &in, const PackageStanza &p, Str &step)
{
    step = "archive";
    Vec<ZipEntry> entries;
    if (zip_entries(in.archive.str(), entries) != ZipRead::Ok)
        co_return Err(Error::Invalid);

    step                 = "metadata";
    const ZipEntry *info = nullptr;
    for (const ZipEntry &e : entries)
        if (zip_meta(e.name) == ZipMeta::PkgInfo)
            info = &e;
    if (!info)
        co_return Err(Error::NotFound);

    Result<String> text = Err(Error::NoMemory);
    if (Task<Result<String>> t = zip_read(*info))
        text = co_await t;
    if (text.is_err())
        co_return Err(text.error());

    step = "pkginfo";
    CO_TRY_VOID(check_meta(entries, p, text.value().str()));

    step = "size";
    if (!check_size(entries, p))
        co_return Err(Error::Unsupported);

    // The directories, in first-seen order; a payload entry's own directory is
    // §8.1's F and the files under it are grouped by it.
    step = "store";
    Vec<Str> dirs;
    for (const ZipEntry &e : entries) {
        if (zip_meta(e.name) != ZipMeta::Payload)
            continue;
        Str dir, leaf;
        db_split(e.name, dir, leaf);
        bool seen = false;
        for (Str had : dirs)
            seen = seen || had == dir;
        if (!seen && !dirs.push(dir))
            co_return Err(Error::NoMemory);
    }

    String root;
    if (!pkg_store_dir(p.name, p.version, "", root))
        co_return Err(Error::NoMemory);

    Vec<StoreOp> ops;
    StoreOp op;
    op.kind = StoreOpKind::Remove;
    if (!op.path.assign(root.str()) || !ops.push(move(op)))
        co_return Err(Error::NoMemory);
    StoreOp mkroot;
    mkroot.kind = StoreOpKind::MkDir;
    if (!mkroot.path.assign(root.str()) || !ops.push(move(mkroot)))
        co_return Err(Error::NoMemory);
    for (Str dir : dirs) {
        String path;
        if (!pkg_store_dir(p.name, p.version, dir, path))
            co_return Err(Error::NoMemory);
        StoreOp mk;
        mk.kind = StoreOpKind::MkDir;
        if (!mk.path.assign(path.str()) || !ops.push(move(mk)))
            co_return Err(Error::NoMemory);
    }
    if (Task<Result<void>> t = store_perform(ops))
        CO_TRY_VOID(co_await t);
    else
        co_return Err(Error::NoMemory);

    if (!in.fresh.push(Installed{ p.name, p.version }))
        co_return Err(Error::NoMemory);

    DbRecord rec;
    rec.pkg           = p;
    rec.index_version = in.index.head.version;

    for (Str dir : dirs) {
        for (const ZipEntry &e : entries) {
            if (zip_meta(e.name) != ZipMeta::Payload)
                continue;
            DbFile file;
            db_split(e.name, file.dir, file.name);
            if (!(file.dir == dir))
                continue;

            Result<String> bytes = Err(Error::NoMemory);
            if (Task<Result<String>> t = zip_read(e))
                bytes = co_await t;
            if (bytes.is_err())
                co_return Err(bytes.error());

            in.hash.reset();
            in.hash.update(bytes.value().str());
            in.hash.finish(file.digest);

            String path;
            if (!pkg_store_dir(p.name, p.version, e.name, path))
                co_return Err(Error::NoMemory);
            if (Task<Result<void>> t = store_write(path.str(), bytes.value().str()))
                CO_TRY_VOID(co_await t);
            else
                co_return Err(Error::NoMemory);

            if (!rec.files.push(file))
                co_return Err(Error::NoMemory);
        }
    }

    // Serialised now: a DbFile views the archive, and the archive goes next.
    String path, out;
    if (!pkg_db_file(p.name, p.version, path) || !db_write(rec, out))
        co_return Err(Error::NoMemory);
    if (!in.record_paths.push(move(path)) || !in.records.push(move(out)))
        co_return Err(Error::NoMemory);
    co_return Result<void>();
}

Task<i32> realise(Txn &in, const PackageStanza &p)
{
    String stem;
    if (!pkg_stem(p.name, p.version, stem))
        co_return 1;

    Result<Have> have = Err(Error::NoMemory);
    if (Task<Result<Have>> t = stem_state(p))
        have = co_await t;
    if (have.is_err()) {
        if (Task<void> e = refuse(stem.str(), "store", have.error()))
            co_await e;
        co_return 1;
    }
    if (have.value() == Have::Other) {
        // §8's store directory is immutable once written, and a live
        // generation may be running out of this one.
        if (Task<void> e = refuse_line(stem.str(), "installed at a different digest"))
            co_await e;
        co_return 1;
    }
    if (have.value() == Have::Same)
        co_return 0;

    IndexStep step   = IndexStep::Package;
    Result<void> got = Err(Error::NoMemory);
    if (Task<Result<void>> t = acquire(in, p, stem.str(), step))
        got = co_await t;
    if (got.is_err()) {
        if (got.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = refuse(stem.str(), index_step_name(step), got.error()))
            co_await e;
        co_return 1;
    }

    // ---- the crossing: these bytes are the index's now (§7's rule) ----
    Str what          = "store";
    Result<void> done = Err(Error::NoMemory);
    if (Task<Result<void>> t = unpack(in, p, what))
        done = co_await t;
    in.archive = String();
    if (done.is_err()) {
        if (done.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = refuse(stem.str(), what, done.error()))
            co_await e;
        co_return 1;
    }
    co_return 0;
}

// A transaction that stops takes its own store directories with it. Nothing
// refers to them: no db record was written and /pkg/active never moved.
Task<void> unwind(Txn &in)
{
    for (const Installed &p : in.fresh)
        if (Task<Result<void>> t = store_drop(p.name, p.version))
            (void)co_await t;
}

// ------------------------------------------------------------- the commit

// The records, then world, then §8.3's generation, ending in the rename.
Task<Result<void>> commit(Txn &in, u32 n)
{
    for (usize i = 0; i < in.records.size(); i++) {
        StoreOp op;
        op.kind = StoreOpKind::Write;
        if (!op.path.assign(in.record_paths[i].str()) || !op.data.assign(in.records[i].str()) ||
            !in.ops.push(move(op)))
            co_return Err(Error::NoMemory);
    }

    String world;
    if (!world_write(in.specs, world))
        co_return Err(Error::NoMemory);
    StoreOp op;
    op.kind = StoreOpKind::Write;
    if (!op.path.assign(PKG_WORLD) || !op.data.assign(world.str()) || !in.ops.push(move(op)))
        co_return Err(Error::NoMemory);

    if (!gen_ops(n, in.want, in.links, in.ops))
        co_return Err(Error::NoMemory);
    if (Task<Result<void>> t = store_perform(in.ops))
        co_return co_await t;
    co_return Err(Error::NoMemory);
}

// -------------------------------------------------- what all three share

// The tree, the index when a command wants one, the installed set, world.
// No index leaves `in.index` empty, which is the repo a removal solves
// against: only what is installed is selectable, so nothing can be fetched.
Task<i32> begin(Txn &in, bool need_index)
{
    i32 bad = 1;
    if (need_index) {
        if (Task<i32> t = pkg_load_index(in.index))
            bad = co_await t;
        if (bad != 0)
            co_return bad;

        // /pkg/cache and /pkg/store must stand before anything writes into
        // them. Only an install builds the tree: a removal writes nothing a
        // generation has not already made, so making one would be the whole
        // of what `pkg remove nonesuch` did.
        Vec<StoreOp> tree;
        Result<void> made = Err(Error::NoMemory);
        if (!pkg_tree_ops(tree))
            co_return 1;
        if (Task<Result<void>> t = store_perform(tree))
            made = co_await t;
        if (made.is_err()) {
            if (Task<void> e = errln("pkg", PKG_DIR, made.error()))
                co_await e;
            co_return 1;
        }
    }

    if (Task<i32> t = read_installed(in))
        bad = co_await t;
    if (bad != 0)
        co_return bad;

    Result<String> world = Err(Error::NoMemory);
    if (Task<Result<String>> t = store_slurp(PKG_WORLD))
        world = co_await t;
    if (world.is_err()) {
        if (Task<void> e = errln("pkg", PKG_WORLD, world.error()))
            co_await e;
        co_return 1;
    }
    in.world_text = move(world.value());
    co_return world_read(in.world_text.str(), in.specs) ? 0 : 1;
}

// The changeset, once the caller has said what it wants of world.
Task<i32> decide(Txn &in, Str lead)
{
    if (!world_deps(in.specs, in.world))
        co_return 1;

    SolveInput si;
    si.repo      = in.index.packages;
    si.installed = in.installed;
    si.world     = in.world;
    si.named     = in.named;
    si.flags     = in.flags;
    if (solve(si, in.cset).is_err())
        co_return 1;

    if (!in.cset.errors.empty()) {
        String out;
        if (!plan_errors(in.cset, lead, out))
            co_return 1;
        if (Task<Result<void>> t = put(SYS_STDERR, out.str()))
            co_await t;
        co_return 1;
    }
    co_return 0;
}

// Nothing to do, but world may still have changed, and that must not be
// silent.
Task<i32> settle_world(Txn &in)
{
    Result<u32> at = Err(Error::NoMemory);
    if (Task<Result<u32>> t = store_active())
        at = co_await t;
    if (at.is_err()) {
        if (Task<void> e = errln("pkg", PKG_ACTIVE, at.error()))
            co_await e;
        co_return 1;
    }

    if (in.world_changed) {
        String text;
        if (!world_write(in.specs, text))
            co_return 1;
        Result<void> w = Err(Error::NoMemory);
        if (Task<Result<void>> t = store_write(PKG_WORLD, text.str()))
            w = co_await t;
        if (w.is_err()) {
            if (Task<void> e = errln("pkg", PKG_WORLD, w.error()))
                co_await e;
            co_return 1;
        }
    }

    Buf<64> line;
    if (at.value() == 0)
        line.put(NOTHING);
    else
        line.put("generation ").put(at.value()).put(", unchanged\n");
    if (Task<Result<void>> t = put(SYS_STDOUT, line.str()))
        co_await t;
    co_return 0;
}

// The plan, the work it names, and the rename that commits it.
Task<i32> settle(Txn &in)
{
    String changes;
    if (!plan_changes(in.cset, changes))
        co_return 1;
    if (changes.empty()) {
        if (Task<i32> t = settle_world(in))
            co_return co_await t;
        co_return 1;
    }

    if (Task<Result<void>> t = put(SYS_STDOUT, changes.str()))
        co_await t;

    // A Purging has no new_pkg: a generation holds what plan_installed names
    // and nothing else, so a removal needs no step of its own.
    for (const SolveChange &c : in.cset.changes) {
        if (!c.new_pkg || plan_verb(c).empty())
            continue;
        i32 bad = 1;
        if (Task<i32> t = realise(in, *c.new_pkg))
            bad = co_await t;
        if (bad != 0) {
            if (Task<void> u = unwind(in))
                co_await u;
            co_return bad;
        }
    }

    if (!plan_installed(in.cset, in.want))
        co_return 1;
    for (const Installed &p : in.want) {
        Result<Vec<String>> got = Err(Error::NoMemory);
        if (Task<Result<Vec<String>>> t = store_commands(p.name, p.version))
            got = co_await t;
        if (got.is_err()) {
            if (Task<void> e = errln("pkg", p.name, got.error()))
                co_await e;
            if (Task<void> u = unwind(in))
                co_await u;
            co_return 1;
        }
        for (String &cmd : got.value()) {
            if (!in.commands.push(move(cmd)))
                co_return 1;
            if (!in.links.push(GenLink{ in.commands.back().str(), p.name, p.version }))
                co_return 1;
        }
    }

    Result<u32> n = Err(Error::NoMemory);
    if (Task<Result<u32>> t = next_gen())
        n = co_await t;
    Result<void> done = Err(Error::NoMemory);
    if (n.is_ok())
        if (Task<Result<void>> t = commit(in, n.value()))
            done = co_await t;
    if (done.is_err()) {
        if (Task<void> e = errln("pkg", PKG_ACTIVE, n.is_err() ? n.error() : done.error()))
            co_await e;
        if (Task<void> u = unwind(in))
            co_await u;
        co_return 1;
    }

    Buf<64> line;
    line.put("generation ")
        .put(n.value())
        .put(", ")
        .put(in.want.size())
        .put(in.want.size() == 1 ? " package\n" : " packages\n");
    if (Task<Result<void>> t = put(SYS_STDOUT, line.str()))
        co_await t;
    co_return 0;
}

// Every operand must be a §6 token; the command's own usage says so.
Task<i32> operands_ok(Args args, Str usage)
{
    for (usize i = 1; i < args.size(); i++) {
        Dep d;
        if (dep_parse(args[i], d) != DepParse::Malformed)
            continue;
        if (Task<Result<void>> t = put(SYS_STDERR, usage))
            co_await t;
        co_return 2;
    }
    co_return 0;
}

// Whether what the transaction leaves installed still provides `name`.
// SOLVE_REMOVE unseats a preference and does not uninstall, so it can. The
// changeset carries the unchanged packages too, which is what makes this
// answerable when there is nothing to print.
bool survives(const Txn &in, Str name)
{
    for (const SolveChange &c : in.cset.changes) {
        if (!c.new_pkg)
            continue;
        if (c.new_pkg->name == name)
            return true;
        Str rest = c.new_pkg->provides, spec;
        while (dep_next(rest, spec)) {
            Dep d;
            if (dep_parse(spec, d) != DepParse::Malformed && d.name == name)
                return true;
        }
    }
    return false;
}

// Whoever still depends on `name`, for the diagnostic.
Str requirer(const Txn &in, Str name)
{
    for (const SolveChange &c : in.cset.changes) {
        if (!c.new_pkg || c.new_pkg->name == name)
            continue;
        Str rest = c.new_pkg->depends, spec;
        while (dep_next(rest, spec)) {
            Dep d;
            if (dep_parse(spec, d) != DepParse::Malformed && d.name == name)
                return c.new_pkg->name;
        }
    }
    return Str();
}

} // namespace

Task<i32> pkg_install(Args args)
{
    if (args.size() < 2) {
        if (Task<Result<void>> t = put(SYS_STDERR, USAGE))
            co_await t;
        co_return 2;
    }
    i32 bad = 1;
    if (Task<i32> t = operands_ok(args, USAGE))
        bad = co_await t;
    if (bad != 0)
        co_return bad;

    Held held{ heap_new<Txn>() };
    if (!held.p) {
        if (Task<Result<void>> t = put(SYS_STDERR, NO_MEMORY))
            co_await t;
        co_return 1;
    }
    Txn &in = *held.p;

    if (Task<i32> t = begin(in, true))
        bad = co_await t;
    if (bad != 0)
        co_return bad;

    // §7 step 7: a name the index does not offer does not exist.
    for (usize i = 1; i < args.size(); i++) {
        Dep d;
        dep_parse(args[i], d);
        if (!index_provides(in.index, d.name)) {
            if (Task<void> e = refuse_line(d.name, "not in the index"))
                co_await e;
            co_return 1;
        }
    }

    // apk's `add`: the operand joins world and names itself, and the run
    // carries no flag of its own.
    for (usize i = 1; i < args.size(); i++) {
        bool changed = false;
        Dep d;
        dep_parse(args[i], d);
        if (!world_push(in.specs, args[i], changed) || !in.named.push(SolveRequest{ d.name, 0, 0 }))
            co_return 1;
        in.world_changed = in.world_changed || changed;
    }

    if (Task<i32> t = decide(in, CANNOT_INSTALL))
        bad = co_await t;
    if (bad != 0)
        co_return bad;
    if (Task<i32> t = settle(in))
        co_return co_await t;
    co_return 1;
}

Task<i32> pkg_remove(Args args)
{
    if (args.size() < 2) {
        if (Task<Result<void>> t = put(SYS_STDERR, USAGE_REMOVE))
            co_await t;
        co_return 2;
    }
    i32 bad = 1;
    if (Task<i32> t = operands_ok(args, USAGE_REMOVE))
        bad = co_await t;
    if (bad != 0)
        co_return bad;

    Held held{ heap_new<Txn>() };
    if (!held.p) {
        if (Task<Result<void>> t = put(SYS_STDERR, NO_MEMORY))
            co_await t;
        co_return 1;
    }
    Txn &in = *held.p;

    if (Task<i32> t = begin(in, false))
        bad = co_await t;
    if (bad != 0)
        co_return bad;

    // apk's `del`: out of world, and unseated where it still stands.
    for (usize i = 1; i < args.size(); i++) {
        Dep d;
        dep_parse(args[i], d);
        if (world_drop(in.specs, d.name))
            in.world_changed = true;
        if (!in.named.push(SolveRequest{ d.name, SOLVE_REMOVE, 0 }))
            co_return 1;
    }

    if (Task<i32> t = decide(in, CANNOT_REMOVE))
        bad = co_await t;
    if (bad != 0)
        co_return bad;

    // Before the commit, apk's order. World is rewritten either way, so a name
    // that stayed has stopped being explicit and has to say so.
    i32 kept = 0;
    for (usize i = 1; i < args.size(); i++) {
        Dep d;
        dep_parse(args[i], d);
        if (!survives(in, d.name))
            continue;
        kept = 1;

        Str who = requirer(in, d.name);
        String tail;
        if (!tail.assign("still needed"))
            co_return 1;
        if (!who.empty() && (!tail.append(" by ") || !tail.append(who)))
            co_return 1;
        if (in.world_changed && !tail.append("; world updated"))
            co_return 1;
        if (Task<void> e = refuse_line(d.name, tail.str()))
            co_await e;
    }

    bad = 1;
    if (Task<i32> t = settle(in))
        bad = co_await t;
    co_return bad != 0 ? bad : kept;
}

Task<i32> pkg_upgrade(Args args)
{
    if (args.size() != 1) {
        if (Task<Result<void>> t = put(SYS_STDERR, USAGE_UPGRADE))
            co_await t;
        co_return 2;
    }

    Held held{ heap_new<Txn>() };
    if (!held.p) {
        if (Task<Result<void>> t = put(SYS_STDERR, NO_MEMORY))
            co_await t;
        co_return 1;
    }
    Txn &in = *held.p;

    // §6's set signed as one file: one solve and one generation, never a
    // loop of installs.
    i32 bad = 1;
    if (Task<i32> t = begin(in, true))
        bad = co_await t;
    if (bad != 0)
        co_return bad;

    in.flags = SOLVE_UPGRADE;
    if (Task<i32> t = decide(in, CANNOT_INSTALL))
        bad = co_await t;
    if (bad != 0)
        co_return bad;
    if (Task<i32> t = settle(in))
        co_return co_await t;
    co_return 1;
}

Task<i32> pkg_autoremove(Args args)
{
    if (args.size() != 1) {
        if (Task<Result<void>> t = put(SYS_STDERR, USAGE_AUTO))
            co_await t;
        co_return 2;
    }

    Held held{ heap_new<Txn>() };
    if (!held.p) {
        if (Task<Result<void>> t = put(SYS_STDERR, NO_MEMORY))
            co_await t;
        co_return 1;
    }
    Txn &in = *held.p;

    // World untouched: what it no longer reaches is what the solver drops.
    i32 bad = 1;
    if (Task<i32> t = begin(in, false))
        bad = co_await t;
    if (bad != 0)
        co_return bad;
    if (Task<i32> t = decide(in, CANNOT_REMOVE))
        bad = co_await t;
    if (bad != 0)
        co_return bad;
    if (Task<i32> t = settle(in))
        co_return co_await t;
    co_return 1;
}

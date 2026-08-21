#include "db.h"

#include "kernel/fmt.h"
#include "version.h"

namespace {

bool join(String &out, Str a, Str b)
{
    return out.assign(a) && (b.empty() || (out.push('/') && out.append(b)));
}

bool push_op(Vec<StoreOp> &out, StoreOpKind kind, Str path, Str data)
{
    StoreOp op;
    op.kind = kind;
    if (!op.path.assign(path) || !op.data.assign(data))
        return false;
    return out.push(move(op));
}

// The next line off `rest`, without its newline. False at the end.
bool line_of(Str &rest, Str &line)
{
    if (rest.empty())
        return false;
    line = rest.split('\n', rest);
    return true;
}

// Str carries only == and !=, and a sort needs an order.
bool before(Str a, Str b)
{
    usize n = a.size() < b.size() ? a.size() : b.size();
    for (usize i = 0; i < n; i++)
        if (a[i] != b[i])
            return u8(a[i]) < u8(b[i]);
    return a.size() < b.size();
}

} // namespace

bool pkg_stem(Str name, Str version, String &out)
{
    return out.assign(name) && out.push('-') && out.append(version);
}

bool pkg_stem_split(Str stem, Str &name, Str &version)
{
    for (usize i = 1; i + 1 < stem.size(); i++) {
        if (stem[i] != '-')
            continue;
        Str tail = stem.substr(i + 1);
        if (!version_valid(tail))
            continue;
        name    = stem.substr(0, i);
        version = tail;
        return true;
    }
    return false;
}

bool pkg_store_dir(Str name, Str version, Str leaf, String &out)
{
    String stem;
    if (!pkg_stem(name, version, stem) || !join(out, PKG_STORE, stem.str()))
        return false;
    return leaf.empty() || (out.push('/') && out.append(leaf));
}

bool pkg_db_file(Str name, Str version, String &out)
{
    String stem;
    return pkg_stem(name, version, stem) && join(out, PKG_DB, stem.str());
}

bool pkg_gen_dir(u32 n, Str leaf, String &out)
{
    Buf<12> num;
    num.put(n);
    if (!join(out, PKG_GEN, num.str()))
        return false;
    return leaf.empty() || (out.push('/') && out.append(leaf));
}

u32 gen_of(Str target)
{
    // The last two components: gen/<n>, however the link was spelled.
    Str rest = target, dir, num;
    while (!rest.empty()) {
        dir = num;
        num = rest.split('/', rest);
    }
    return dir == "gen" ? gen_number(num) : 0;
}

u32 gen_number(Str name)
{
    if (name.empty() || name.size() > 9)
        return 0;

    u32 n = 0;
    for (char c : name) {
        if (c < '0' || c > '9')
            return 0;
        n = n * 10 + u32(c - '0');
    }
    return n;
}

bool gen_keep(Span<const u32> gens, u32 active, Vec<u32> &keep)
{
    // The one to roll back to: the highest below the active one.
    u32 previous = 0;
    for (u32 n : gens)
        if (n < active && n > previous)
            previous = n;

    // Ascending, whatever order the listing arrived in.
    for (u32 n : gens) {
        if (n != active && n != previous && n < active)
            continue;
        usize at = keep.size();
        while (at > 0 && keep[at - 1] > n)
            at--;
        if (!keep.insert(at, n))
            return false;
    }
    return true;
}

bool packages_write(Span<const Installed> v, String &out)
{
    // An insertion sort over the indices: a generation is a handful of names.
    Vec<usize> order;
    for (usize i = 0; i < v.size(); i++) {
        usize at = order.size();
        while (at > 0 && before(v[i].name, v[order[at - 1]].name))
            at--;
        if (!order.insert(at, i))
            return false;
    }

    for (usize i : order) {
        if (v[i].name.empty() || v[i].version.empty())
            return false;
        if (!out.append(v[i].name) || !out.push(' ') || !out.append(v[i].version) ||
            !out.push('\n'))
            return false;
    }
    return true;
}

bool packages_read(Str text, Vec<Installed> &out)
{
    Str rest = text, line;
    while (line_of(rest, line)) {
        if (line.empty())
            continue;

        Installed p;
        p.name = line.split(' ', line);
        if (p.name.empty())
            return false;
        p.version = line.split(' ', line);
        if (p.version.empty() || !line.empty())
            return false;
        if (!out.push(p))
            return false;
    }
    return true;
}

Str installed_version(Str text, Str name)
{
    Vec<Installed> pkgs;
    if (!packages_read(text, pkgs))
        return Str();
    for (const Installed &p : pkgs)
        if (p.name == name)
            return p.version;
    return Str();
}

bool world_write(Span<const Str> deps, String &out)
{
    for (Str d : deps) {
        if (d.empty() || !out.append(d) || !out.push('\n'))
            return false;
    }
    return true;
}

bool world_read(Str text, Vec<Str> &out)
{
    Str rest = text, line;
    while (line_of(rest, line)) {
        if (line.empty())
            continue;
        if (!out.push(line))
            return false;
    }
    return true;
}

bool world_push(Vec<Str> &specs, Str spec, bool &changed)
{
    changed = false;

    Dep want;
    if (dep_parse(spec, want) == DepParse::Malformed)
        return true;

    for (Str &had : specs) {
        Dep d;
        if (dep_parse(had, d) == DepParse::Malformed || d.name != want.name)
            continue;
        changed = !(had == spec);
        had     = spec;
        return true;
    }
    changed = true;
    return specs.push(spec);
}

bool world_drop(Vec<Str> &specs, Str name)
{
    // Every line naming it, not the first: a hand-edited world may say a name
    // twice, and leaving the second would make the package unremovable.
    bool found = false;
    for (usize i = specs.size(); i > 0; i--) {
        Dep d;
        if (dep_parse(specs[i - 1], d) == DepParse::Malformed || d.name != name)
            continue;
        specs.erase(i - 1);
        found = true;
    }
    return found;
}

bool world_deps(Span<const Str> specs, Vec<Dep> &out)
{
    for (Str spec : specs) {
        Dep d;
        if (dep_parse(spec, d) == DepParse::Malformed)
            continue;
        if (!out.push(d))
            return false;
    }
    return true;
}

bool repos_read(Str text, Vec<Str> &out)
{
    return world_read(text, out);
}

void db_split(Str entry, Str &dir, Str &name)
{
    usize at = Str::npos;
    for (usize i = 0; i < entry.size(); i++)
        if (entry[i] == '/')
            at = i;
    if (at == Str::npos) {
        dir  = Str();
        name = entry;
        return;
    }
    dir  = entry.substr(0, at);
    name = entry.substr(at + 1);
}

bool db_join(Str dir, Str name, String &out)
{
    return dir.empty() ? out.assign(name) : join(out, dir, name);
}

bool pkg_tree_ops(Vec<StoreOp> &out)
{
    constexpr Str DIRS[] = { PKG_DIR, PKG_STORE, PKG_DB, PKG_GEN, PKG_CACHE };
    for (Str d : DIRS)
        if (!push_op(out, StoreOpKind::MkDir, d, ""))
            return false;

    String target;
    return join(target, PKG_ACTIVE, "bin") &&
           push_op(out, StoreOpKind::Link, PKG_BIN, target.str());
}

bool gen_ops(u32 n, Span<const Installed> pkgs, Span<const GenLink> links, Vec<StoreOp> &out)
{
    String dir, text;
    if (!pkg_gen_dir(n, "", dir) || !packages_write(pkgs, text))
        return false;

    // Whatever a tab that died left behind; pkg clean is for the store, not
    // for a generation about to be rebuilt under its own number.
    if (!push_op(out, StoreOpKind::Remove, dir.str(), "") ||
        !push_op(out, StoreOpKind::MkDir, dir.str(), ""))
        return false;

    String path;
    if (!pkg_gen_dir(n, "packages", path) ||
        !push_op(out, StoreOpKind::Write, path.str(), text.str()))
        return false;
    if (!pkg_gen_dir(n, "bin", path) || !push_op(out, StoreOpKind::MkDir, path.str(), ""))
        return false;

    for (const GenLink &l : links) {
        String leaf, target;
        if (l.command.empty() || !leaf.assign("bin/") || !leaf.append(l.command))
            return false;
        if (!pkg_gen_dir(n, leaf.str(), path) ||
            !pkg_store_dir(l.name, l.version, leaf.str(), target))
            return false;
        if (!push_op(out, StoreOpKind::Link, path.str(), target.str()))
            return false;
    }

    return push_op(out, StoreOpKind::Link, PKG_ACTIVE_NEW, dir.str()) &&
           push_op(out, StoreOpKind::Rename, PKG_ACTIVE_NEW, PKG_ACTIVE);
}

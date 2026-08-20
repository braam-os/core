#include "solve.h"

#include "kernel/hash.h"
#include "kernel/traits.h"

// apk-tools/src/solver.c, ported. Its function names are kept so the two can
// be read side by side; pinning and repository tags are gone throughout.

namespace {

constexpr u32 NONE = ~u32(0);

// A package index paired with the version it provides a name under. An empty
// `version` is apk's &apk_atom_null: provided, but unversioned.
struct SolveProvider {
    u32 pkg = NONE;
    Str version;

    bool unversioned() const { return version.empty(); }
};

struct SolvePkg {
    const PackageStanza *stanza = nullptr;
    u32 name                    = NONE;
    Vec<Dep> depends, provides, install_if;
    u32 priority = 0;
    bool ipkg    = false; // in the installed set
    bool repo    = false; // in the index

    u32 conflicts   = 0;
    u32 flags       = 0;
    u32 inheritable = 0;
    bool seen = false, available = false, selectable = false;
    bool iif_triggered = false, iif_failed = false;
    bool deps_merged = false, deps_used = false;
    bool in_changeset = false, error = false;
    bool direct_error = false; // a cause, not something a cause propagated to
    u32 walk          = 0;     // reverse-dependency walk generation
};

// Which list a name is parked on. apk uses one intrusive link for the three.
enum class Queue { None, ResolveNow, Selectable, Unresolved };

struct SolveName {
    Str name;
    Vec<SolveProvider> providers;
    Vec<u32> rdepends, rinstall_if;

    SolveProvider chosen;
    u32 installed_pkg  = NONE; // changeset generation only
    u32 installed_name = NONE;

    i32 order_id      = 0;
    u32 requirers     = 0;
    u32 merge_depends = 0, merge_provides = 0;

    Queue queue = Queue::None;
    u32 prev = NONE, next = NONE;
    bool dirty_queued = false;
    u32 dirty_next    = NONE;

    bool seen = false, locked = false;
    bool changeset_processed = false, changeset_removed = false;
    bool reevaluate_deps = false, reevaluate_iif = false;
    bool has_iif = false, no_iif = false, has_options = false;
    bool reverse_deps_done = false, has_virtual_provides = false;
    bool has_auto_selectable = false, iif_needed = false, resolvenow = false;
    bool requested = false;
};

struct Solver {
    Vec<SolvePkg> pkgs;
    Vec<SolveName> names;
    HashMap<Str, u32> index;
    Changeset *cset = nullptr;
    u32 flags       = 0;
    u32 inherit     = 0;

    u32 dirty_head = NONE, dirty_tail = NONE;
    u32 resolvenow_head = NONE, resolvenow_tail = NONE;
    u32 selectable_head = NONE, unresolved_head = NONE;

    u32 order  = 0;
    u32 errors = 0;
    bool oom   = false;

    SolvePkg &pkg(u32 i) { return pkgs[i]; }
    SolveName &nm(u32 i) { return names[i]; }
};

// Every allocation failure funnels here; the solve runs on to completion and
// solve() reports NoMemory, which keeps the call sites free of TRY.
bool fail(Solver &s)
{
    s.oom = true;
    return false;
}

u32 intern(Solver &s, Str name)
{
    if (u32 *at = s.index.find(name))
        return *at;
    SolveName n;
    n.name = name;
    u32 i  = u32(s.names.size());
    if (!s.names.push(move(n)) || !s.index.insert(name, i)) {
        fail(s);
        return NONE;
    }
    return i;
}

bool parse_list(Solver &s, Str text, Vec<Dep> &out)
{
    Str rest = text, spec;
    while (dep_next(rest, spec)) {
        Dep d;
        if (dep_parse(spec, d) == DepParse::Malformed)
            continue;
        if (!out.push(d))
            return fail(s);
    }
    return true;
}

// apk_name_cmp_display: case-insensitively, then exactly. World is sorted by
// it, which is what fixes the discovery order and so every decision after it.
char fold_ascii(char c)
{
    return c >= 'A' && c <= 'Z' ? char(c + 32) : c;
}

int name_cmp(Str a, Str b)
{
    usize n = a.size() < b.size() ? a.size() : b.size();
    for (usize i = 0; i < n; i++) {
        char x = fold_ascii(a[i]), y = fold_ascii(b[i]);
        if (x != y)
            return x < y ? -1 : 1;
    }
    if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
    for (usize i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

// ------------------------------------------------------- building the graph

bool add_package(Solver &s, const PackageStanza *st, bool installed)
{
    SolvePkg p;
    p.stanza   = st;
    p.ipkg     = installed;
    p.repo     = !installed;
    p.priority = st->priority;
    if (!parse_list(s, st->depends, p.depends) || !parse_list(s, st->provides, p.provides) ||
        !parse_list(s, st->install_if, p.install_if))
        return false;

    u32 self = intern(s, st->name);
    if (self == NONE)
        return false;
    p.name = self;

    u32 i = u32(s.pkgs.size());
    if (!s.pkgs.push(move(p)))
        return fail(s);

    if (!s.names[self].providers.push(SolveProvider{ i, st->version }))
        return fail(s);
    for (const Dep &d : s.pkgs[i].provides) {
        u32 n = intern(s, d.name);
        if (n == NONE)
            return false;
        if (!s.names[n].providers.push(SolveProvider{ i, d.version }))
            return fail(s);
    }
    return true;
}

bool add_rname(Solver &s, Vec<u32> &to, u32 name)
{
    for (u32 n : to)
        if (n == name)
            return true;
    return to.push(name) || fail(s);
}

// A dependency's name reverse-depends on the requiring package's own name and
// on every name it provides.
bool build_reverse(Solver &s)
{
    for (u32 i = 0; i < s.pkgs.size(); i++) {
        SolvePkg &p = s.pkgs[i];
        for (const Dep &d : p.depends) {
            u32 n = intern(s, d.name);
            if (n == NONE || !add_rname(s, s.names[n].rdepends, p.name))
                return false;
            for (const Dep &q : p.provides) {
                u32 pn = intern(s, q.name);
                if (pn == NONE || !add_rname(s, s.names[n].rdepends, pn))
                    return false;
            }
        }
        for (const Dep &d : p.install_if) {
            u32 n = intern(s, d.name);
            if (n == NONE || !add_rname(s, s.names[n].rinstall_if, p.name))
                return false;
            for (const Dep &q : p.provides) {
                u32 pn = intern(s, q.name);
                if (pn == NONE || !add_rname(s, s.names[n].rinstall_if, pn))
                    return false;
            }
        }
    }
    return true;
}

// ------------------------------------------------------------- dep matching

// apk_dep_is_provided: nothing selected satisfies a conflict and fails the
// rest, and a package never conflicts with itself.
bool dep_provided(u32 dpkg, const Dep &d, const SolveProvider &p)
{
    bool conflict = (d.mask & VER_CONFLICT) != 0;
    if (p.pkg == NONE)
        return conflict;
    if (conflict && dpkg == p.pkg)
        return true;
    return dep_satisfied(d, p.version);
}

// ------------------------------------------------------------- the queues

void queue_remove(Solver &s, u32 i)
{
    SolveName &n = s.nm(i);
    if (n.queue == Queue::None)
        return;
    if (n.prev != NONE)
        s.nm(n.prev).next = n.next;
    if (n.next != NONE)
        s.nm(n.next).prev = n.prev;

    u32 *head = n.queue == Queue::ResolveNow   ? &s.resolvenow_head
                : n.queue == Queue::Selectable ? &s.selectable_head
                                               : &s.unresolved_head;
    if (*head == i)
        *head = n.next;
    if (n.queue == Queue::ResolveNow && s.resolvenow_tail == i)
        s.resolvenow_tail = n.prev;
    n.prev = n.next = NONE;
    n.queue         = Queue::None;
}

void queue_fifo(Solver &s, u32 i)
{
    SolveName &n = s.nm(i);
    n.queue      = Queue::ResolveNow;
    n.prev       = s.resolvenow_tail;
    n.next       = NONE;
    if (s.resolvenow_tail != NONE)
        s.nm(s.resolvenow_tail).next = i;
    else
        s.resolvenow_head = i;
    s.resolvenow_tail = i;
}

// Insertion sort, order_id descending. The key is signed: apk sets the top bit
// on a name nobody asked for, so it sorts behind one that was asked for.
void queue_insert(Solver &s, u32 head_of, u32 i)
{
    u32 *head    = head_of == 0 ? &s.selectable_head : &s.unresolved_head;
    SolveName &n = s.nm(i);
    n.queue      = head_of == 0 ? Queue::Selectable : Queue::Unresolved;

    u32 at = *head, prev = NONE;
    while (at != NONE && s.nm(at).order_id >= n.order_id) {
        prev = at;
        at   = s.nm(at).next;
    }
    n.prev = prev;
    n.next = at;
    if (at != NONE)
        s.nm(at).prev = i;
    if (prev != NONE)
        s.nm(prev).next = i;
    else
        *head = i;
}

void queue_dirty(Solver &s, u32 i)
{
    SolveName &n = s.nm(i);
    if (n.dirty_queued || n.locked || (n.requirers == 0 && !n.reevaluate_iif))
        return;
    n.dirty_queued = true;
    n.dirty_next   = NONE;
    if (s.dirty_tail != NONE)
        s.nm(s.dirty_tail).dirty_next = i;
    else
        s.dirty_head = i;
    s.dirty_tail = i;
}

// Every constraint that could ever apply has applied, somebody wants it, and
// there is exactly one thing left to pick: apk's unit propagation.
bool queue_resolvenow(const SolveName &n)
{
    return n.reverse_deps_done && n.requirers && n.has_auto_selectable && !n.has_options;
}

void queue_unresolved(Solver &s, u32 i, bool reevaluate)
{
    SolveName &n = s.nm(i);
    if (n.locked)
        return;
    if (n.queue != Queue::None) {
        if (n.resolvenow)
            return;
        if (queue_resolvenow(n))
            n.resolvenow = true;
        else if (!reevaluate)
            return;
        queue_remove(s, i);
    } else {
        if (n.requirers == 0 && !n.has_iif && !n.iif_needed)
            return;
        n.resolvenow = queue_resolvenow(n);
    }

    if (s.nm(i).resolvenow)
        queue_fifo(s, i);
    else
        queue_insert(s, s.nm(i).has_auto_selectable ? 0 : 1, i);
}

u32 dequeue_next_name(Solver &s)
{
    u32 i = s.resolvenow_head != NONE   ? s.resolvenow_head
            : s.selectable_head != NONE ? s.selectable_head
                                        : s.unresolved_head;
    if (i != NONE)
        queue_remove(s, i);
    return i;
}

// ------------------------------------------------------------ waking things

void reevaluate_reverse_deps(Solver &s, u32 name)
{
    for (u32 n0 : s.nm(name).rdepends) {
        if (!s.nm(n0).seen)
            continue;
        s.nm(n0).reevaluate_deps = true;
        queue_dirty(s, n0);
    }
}

void reevaluate_reverse_installif(Solver &s, u32 name)
{
    for (u32 n0 : s.nm(name).rinstall_if) {
        if (!s.nm(n0).seen || s.nm(n0).no_iif)
            continue;
        s.nm(n0).reevaluate_iif = true;
        queue_dirty(s, n0);
    }
}

void reevaluate_reverse_installif_pkg(Solver &s, u32 pkg)
{
    reevaluate_reverse_installif(s, s.pkg(pkg).name);
    for (const Dep &d : s.pkg(pkg).provides)
        if (u32 *n = s.index.find(d.name))
            reevaluate_reverse_installif(s, *n);
}

void disqualify_package(Solver &s, u32 pkg)
{
    s.pkg(pkg).selectable = false;
    reevaluate_reverse_deps(s, s.pkg(pkg).name);
    for (const Dep &d : s.pkg(pkg).provides)
        if (u32 *n = s.index.find(d.name))
            reevaluate_reverse_deps(s, *n);
    reevaluate_reverse_installif_pkg(s, pkg);
}

void mark_error(Solver &s, u32 pkg)
{
    if (pkg == NONE || s.pkg(pkg).error)
        return;
    s.pkg(pkg).error = true;
    s.errors++;
}

// --------------------------------------------------------------- discovery

void discover_name(Solver &s, u32 i);

void discover_dep_names(Solver &s, const Vec<Dep> &deps)
{
    for (const Dep &d : deps) {
        u32 n = intern(s, d.name);
        if (n != NONE)
            discover_name(s, n);
    }
}

void discover_name(Solver &s, u32 i)
{
    if (s.nm(i).seen)
        return;
    s.nm(i).seen   = true;
    s.nm(i).no_iif = true;

    u32 virtuals = 0;
    for (usize k = 0; k < s.nm(i).providers.size(); k++) {
        u32 pi = s.nm(i).providers[k].pkg;
        if (!s.pkg(pi).seen) {
            SolvePkg &p = s.pkg(pi);
            p.seen      = true;
            p.available = p.repo;
            // A meta-package has nothing to download, and something already
            // installed is already here.
            p.selectable = p.available || p.stanza->installed_size == 0 || p.ipkg;
            p.iif_failed = p.install_if.empty() || ((s.inherit & SOLVE_AVAILABLE) && !p.available);
            discover_dep_names(s, p.depends);
        }
        s.nm(i).no_iif = s.nm(i).no_iif && s.pkg(pi).iif_failed;
        virtuals += s.pkg(pi).name != i;
    }

    for (usize k = 0; k < s.nm(i).rinstall_if.size(); k++)
        discover_name(s, s.nm(i).rinstall_if[k]);

    for (usize k = 0; k < s.nm(i).providers.size(); k++) {
        u32 pi = s.nm(i).providers[k].pkg;
        for (usize j = 0; j < s.nm(s.pkg(pi).name).rinstall_if.size(); j++)
            discover_name(s, s.nm(s.pkg(pi).name).rinstall_if[j]);
        discover_dep_names(s, s.pkg(pi).provides);
    }

    // Bit 31 for a name nobody asked for, bit 30 for one a real package
    // carries; the counter below them numbers children before parents.
    u32 bits = 0;
    if (!s.nm(i).requested)
        bits |= 1u << 31;
    if (s.nm(i).providers.size() != virtuals)
        bits |= 1u << 30;
    s.nm(i).order_id = i32(bits | ++s.order);

    for (usize k = 0; k < s.nm(i).providers.size(); k++)
        discover_dep_names(s, s.pkg(s.nm(i).providers[k].pkg).install_if);
}

// -------------------------------------------------------------- constraints

void inherit_flags(Solver &s, u32 pkg, u32 ppkg)
{
    SolvePkg &p = s.pkg(pkg);
    if (ppkg != NONE) {
        p.flags |= s.pkg(ppkg).inheritable;
        p.inheritable |= s.pkg(ppkg).inheritable;
    } else {
        p.flags |= s.inherit;
        p.inheritable |= s.inherit;
    }
}

void name_requirers_changed(Solver &s, u32 name)
{
    queue_unresolved(s, name, false);
    reevaluate_reverse_installif(s, name);
    queue_dirty(s, name);
}

void apply_constraint(Solver &s, u32 ppkg, const Dep &d)
{
    u32 *at = s.index.find(d.name);
    if (!at)
        return;
    u32 name      = *at;
    bool conflict = (d.mask & VER_CONFLICT) != 0;

    s.nm(name).requirers += !conflict;
    if (s.nm(name).requirers == 1 && !conflict)
        name_requirers_changed(s, name);

    for (usize k = 0; k < s.nm(name).providers.size(); k++) {
        SolveProvider p = s.nm(name).providers[k];
        bool provided   = dep_provided(ppkg, d, p);
        s.pkg(p.pkg).conflicts += !provided;
        if (s.pkg(p.pkg).selectable && s.pkg(p.pkg).conflicts)
            disqualify_package(s, p.pkg);
        if (provided)
            inherit_flags(s, p.pkg, ppkg);
    }
}

bool dependency_satisfiable(Solver &s, u32 dpkg, const Dep &d)
{
    u32 *at = s.index.find(d.name);
    if (!at)
        return (d.mask & VER_CONFLICT) != 0;
    SolveName &n = s.nm(*at);

    if (n.locked)
        return dep_provided(dpkg, d, n.chosen);
    if (n.requirers == 0 && dep_provided(dpkg, d, SolveProvider{}))
        return true;
    for (usize k = 0; k < n.providers.size(); k++)
        if (s.pkg(n.providers[k].pkg).selectable && dep_provided(dpkg, d, n.providers[k]))
            return true;
    return false;
}

// Whatever satisfies `must_provide` also provides `name`, so a provider of
// `name` that does not is a second claim on it.
void exclude_non_providers(Solver &s, u32 name, u32 must_provide, bool skip_virtuals)
{
    if (name == must_provide)
        return;
    for (usize k = 0; k < s.nm(name).providers.size(); k++) {
        SolveProvider p = s.nm(name).providers[k];
        if (s.pkg(p.pkg).name == must_provide || !s.pkg(p.pkg).selectable ||
            (skip_virtuals && p.unversioned()))
            continue;
        bool ok = false;
        for (const Dep &d : s.pkg(p.pkg).provides) {
            u32 *n = s.index.find(d.name);
            if ((n && *n == must_provide) || (skip_virtuals && d.version.empty())) {
                ok = true;
                break;
            }
        }
        if (!ok)
            disqualify_package(s, p.pkg);
    }
}

// An intersection counter: it advances only for a name every candidate so far
// has named, so after the loop `== num_options` means all of them did.
bool merge_index(u32 &index, u32 num_options)
{
    if (index != num_options)
        return false;
    index = num_options + 1;
    return true;
}

bool merge_index_complete(u32 &index, u32 num_options)
{
    bool got = index == num_options;
    index    = 0;
    return got;
}

// A bare virtual provide is never picked on its own: it needs a version, a
// priority, or its own package name to be wanted.
bool is_provider_auto_selectable(Solver &s, const SolveProvider &p)
{
    if (!p.unversioned())
        return true;
    if (s.pkg(p.pkg).priority)
        return true;
    return s.nm(s.pkg(p.pkg).name).requirers != 0;
}

void reconsider_name(Solver &s, u32 i)
{
    u32 first_candidate = NONE;
    u32 num_options     = 0;
    bool has_iif = false, no_iif = true, has_auto_selectable = false;
    bool reevaluate = false;

    bool reevaluate_deps    = s.nm(i).reevaluate_deps;
    bool reevaluate_iif     = s.nm(i).reevaluate_iif;
    s.nm(i).reevaluate_deps = false;
    s.nm(i).reevaluate_iif  = false;

    for (usize k = 0; k < s.nm(i).providers.size(); k++) {
        SolveProvider p       = s.nm(i).providers[k];
        u32 pi                = p.pkg;
        s.pkg(pi).deps_merged = false;

        if (reevaluate_deps) {
            if (!s.pkg(pi).selectable)
                continue;
            for (const Dep &d : s.pkg(pi).depends) {
                if (!dependency_satisfiable(s, pi, d)) {
                    disqualify_package(s, pi);
                    break;
                }
            }
        }
        if (!s.pkg(pi).selectable)
            continue;

        if (reevaluate_iif && !s.pkg(pi).iif_triggered && !s.pkg(pi).iif_failed) {
            s.pkg(pi).iif_triggered = true;
            for (const Dep &d : s.pkg(pi).install_if) {
                u32 *at = s.index.find(d.name);
                if (!at || !s.nm(*at).locked) {
                    if (at && (d.mask & VER_CONFLICT)) {
                        s.nm(*at).iif_needed = true;
                        queue_unresolved(s, *at, false);
                    }
                    s.pkg(pi).iif_triggered = false;
                    s.pkg(pi).iif_failed    = false;
                    break;
                }
                if (!dep_provided(pi, d, s.nm(*at).chosen)) {
                    s.pkg(pi).iif_triggered = false;
                    s.pkg(pi).iif_failed    = true;
                    break;
                }
            }
        }
        if (reevaluate_iif && s.pkg(pi).iif_triggered) {
            for (const Dep &d : s.pkg(pi).install_if)
                if (u32 *at = s.index.find(d.name))
                    inherit_flags(s, pi, s.nm(*at).chosen.pkg);
        }
        has_iif             = has_iif || s.pkg(pi).iif_triggered;
        no_iif              = no_iif && s.pkg(pi).iif_failed;
        has_auto_selectable = has_auto_selectable || s.pkg(pi).iif_triggered;

        if (s.nm(i).requirers == 0)
            continue;

        s.pkg(pi).deps_merged = true;
        if (first_candidate == NONE)
            first_candidate = pi;

        for (const Dep &d : s.pkg(pi).depends)
            if (!(d.mask & VER_CONFLICT))
                if (u32 *at = s.index.find(d.name))
                    merge_index(s.nm(*at).merge_depends, num_options);

        if (merge_index(s.nm(s.pkg(pi).name).merge_provides, num_options))
            s.nm(s.pkg(pi).name).has_virtual_provides =
                s.nm(s.pkg(pi).name).has_virtual_provides || p.unversioned();
        for (const Dep &d : s.pkg(pi).provides)
            if (u32 *at = s.index.find(d.name))
                if (merge_index(s.nm(*at).merge_provides, num_options))
                    s.nm(*at).has_virtual_provides =
                        s.nm(*at).has_virtual_provides || d.version.empty();

        num_options++;
        if (!has_auto_selectable && is_provider_auto_selectable(s, p))
            has_auto_selectable = true;
    }

    s.nm(i).has_options = num_options > 1;
    s.nm(i).has_iif     = has_iif;
    s.nm(i).no_iif      = no_iif;
    if (has_auto_selectable != s.nm(i).has_auto_selectable) {
        s.nm(i).has_auto_selectable = has_auto_selectable;
        reevaluate                  = true;
    }

    if (first_candidate != NONE) {
        u32 pi = first_candidate;
        for (usize k = 0; k < s.nm(i).providers.size(); k++) {
            u32 p0              = s.nm(i).providers[k].pkg;
            s.pkg(p0).deps_used = s.pkg(p0).deps_merged;
        }

        if (num_options == 1) {
            // One candidate left: its dependencies are certain, versions and
            // all. This is the propagation that drives the solve.
            for (usize k = 0; k < s.pkg(pi).depends.size(); k++) {
                Dep d = s.pkg(pi).depends[k];
                if (u32 *at = s.index.find(d.name))
                    if (merge_index_complete(s.nm(*at).merge_depends, num_options))
                        apply_constraint(s, pi, d);
            }
        } else {
            // Several left, but a name all of them want is wanted: bump it,
            // without a version nobody has agreed on yet.
            for (usize k = 0; k < s.pkg(pi).depends.size(); k++) {
                Dep d   = s.pkg(pi).depends[k];
                u32 *at = s.index.find(d.name);
                if (!at)
                    continue;
                u32 n0 = *at;
                if (merge_index_complete(s.nm(n0).merge_depends, num_options) &&
                    s.nm(n0).requirers == 0) {
                    s.nm(n0).requirers++;
                    name_requirers_changed(s, n0);
                    for (usize j = 0; j < s.nm(n0).providers.size(); j++)
                        inherit_flags(s, s.nm(n0).providers[j].pkg, pi);
                }
            }
        }

        u32 own = s.pkg(pi).name;
        if (merge_index_complete(s.nm(own).merge_provides, num_options))
            exclude_non_providers(s, own, i, s.nm(own).has_virtual_provides);
        for (usize k = 0; k < s.pkg(pi).provides.size(); k++) {
            Dep d = s.pkg(pi).provides[k];
            if (u32 *at = s.index.find(d.name))
                if (merge_index_complete(s.nm(*at).merge_provides, num_options))
                    exclude_non_providers(s, *at, i, s.nm(*at).has_virtual_provides);
        }

        s.nm(own).has_virtual_provides = false;
        for (const Dep &d : s.pkg(pi).provides)
            if (u32 *at = s.index.find(d.name))
                s.nm(*at).has_virtual_provides = false;
    }

    s.nm(i).reverse_deps_done = true;
    for (u32 n0 : s.nm(i).rdepends) {
        if (s.nm(n0).seen && !s.nm(n0).locked) {
            s.nm(i).reverse_deps_done = false;
            break;
        }
    }
    queue_unresolved(s, i, reevaluate);
}

// ---------------------------------------------------------------- choosing

i32 prefer(bool a, bool b)
{
    return i32(a) - i32(b);
}

// A strict tiebreak chain; >0 keeps A. Ties keep whichever came first in the
// provider array, since select_package only replaces on >0.
i32 compare_providers(Solver &s, const SolveProvider &pa, const SolveProvider &pb)
{
    if (pa.pkg == NONE || pb.pkg == NONE)
        return prefer(pa.pkg != NONE, pb.pkg != NONE);

    SolvePkg &a = s.pkg(pa.pkg);
    SolvePkg &b = s.pkg(pb.pkg);
    u32 flags   = a.flags | b.flags;
    i32 r;

    if (flags & SOLVE_LATEST) {
        // Version wins over everything, breakage included: the report is what
        // explains it.
        if (flags & SOLVE_AVAILABLE) {
            if ((r = prefer(a.available, b.available)))
                return r;
        } else if (flags & SOLVE_REINSTALL) {
            if ((r = prefer(a.selectable, b.selectable)))
                return r;
        }
    } else {
        if ((r = prefer(a.selectable, b.selectable)))
            return r;
        if ((r = prefer(a.deps_used, b.deps_used)))
            return r;
        if ((r = i32(b.conflicts) - i32(a.conflicts)))
            return r;
        if (flags & SOLVE_INSTALLED)
            if ((r = prefer(a.ipkg, b.ipkg)))
                return r;
        if (flags & SOLVE_AVAILABLE)
            if ((r = prefer(a.available, b.available)))
                return r;
        // Not while removing or upgrading, and not between two packages that
        // merely both provide a bare name.
        if (!(flags & (SOLVE_REMOVE | SOLVE_UPGRADE)) &&
            (a.name == b.name || !pa.unversioned() || !pb.unversioned()))
            if ((r = prefer(a.ipkg, b.ipkg)))
                return r;
    }

    u32 cmp = version_compare(pa.version, pb.version);
    if (cmp == VER_LESS)
        return -1;
    if (cmp == VER_GREATER)
        return 1;

    if (a.name == b.name) {
        cmp = version_compare(a.stanza->version, b.stanza->version);
        if (cmp == VER_LESS)
            return -1;
        if (cmp == VER_GREATER)
            return 1;
    }

    if ((r = i32(a.priority) - i32(b.priority)))
        return r;
    if (!(flags & SOLVE_REMOVE))
        if ((r = prefer(a.ipkg, b.ipkg)))
            return r;
    if ((r = prefer(a.selectable, b.selectable)))
        return r;
    // apk's lowest-repository rung: an installed-only package is in none.
    return prefer(b.available, a.available);
}

void assign_name(Solver &s, u32 name, const SolveProvider &p)
{
    SolveName &n = s.nm(name);
    if (n.locked) {
        // Two bare provides of one name may coexist; anything else is a
        // second claim, and both packages answer for it.
        if (p.unversioned() && n.chosen.unversioned())
            return;
        mark_error(s, p.pkg);
        mark_error(s, n.chosen.pkg);
        if (p.pkg != NONE)
            s.pkg(p.pkg).direct_error = true;
        if (n.chosen.pkg != NONE)
            s.pkg(n.chosen.pkg).direct_error = true;
        return;
    }

    n.locked = true;
    n.chosen = p;
    queue_remove(s, name);

    if (p.pkg != NONE && !n.requirers && s.pkg(p.pkg).iif_triggered) {
        for (usize k = 0; k < s.pkg(p.pkg).install_if.size(); k++) {
            Dep d = s.pkg(p.pkg).install_if[k];
            if (u32 *at = s.index.find(d.name))
                if (!s.nm(*at).locked)
                    apply_constraint(s, p.pkg, d);
        }
    }

    for (usize k = 0; k < s.nm(name).providers.size(); k++) {
        SolveProvider p0 = s.nm(name).providers[k];
        if (p0.pkg == p.pkg)
            continue;
        if (p.unversioned() && p0.unversioned())
            continue;
        disqualify_package(s, p0.pkg);
    }
    reevaluate_reverse_deps(s, name);
    if (p.pkg != NONE)
        reevaluate_reverse_installif_pkg(s, p.pkg);
    else
        reevaluate_reverse_installif(s, name);
}

void select_package(Solver &s, u32 i)
{
    SolveProvider chosen;

    if (s.nm(i).requirers || s.nm(i).has_iif) {
        for (usize k = 0; k < s.nm(i).providers.size(); k++) {
            SolveProvider p = s.nm(i).providers[k];
            if (s.nm(i).requirers == 0 && (!s.pkg(p.pkg).iif_triggered || !s.pkg(p.pkg).selectable))
                continue;
            if (!is_provider_auto_selectable(s, p))
                continue;
            if (compare_providers(s, p, chosen) > 0)
                chosen = p;
        }
    }

    if (chosen.pkg == NONE) {
        assign_name(s, i, SolveProvider{});
        if (s.nm(i).requirers > 0)
            s.errors++;
        return;
    }

    if (!s.pkg(chosen.pkg).selectable)
        mark_error(s, chosen.pkg);

    u32 pi = chosen.pkg;
    assign_name(s, s.pkg(pi).name, SolveProvider{ pi, s.pkg(pi).stanza->version });
    for (usize k = 0; k < s.pkg(pi).provides.size(); k++) {
        Dep d = s.pkg(pi).provides[k];
        if (u32 *at = s.index.find(d.name))
            assign_name(s, *at, SolveProvider{ pi, d.version });
    }
    for (usize k = 0; k < s.pkg(pi).depends.size(); k++)
        apply_constraint(s, pi, s.pkg(pi).depends[k]);
}

// ------------------------------------------------------------- the changeset

void record_change(Solver &s, u32 opkg, u32 npkg)
{
    SolveChange c;
    c.old_pkg   = opkg == NONE ? nullptr : s.pkg(opkg).stanza;
    c.new_pkg   = npkg == NONE ? nullptr : s.pkg(npkg).stanza;
    c.reinstall = npkg != NONE && (s.pkg(npkg).flags & SOLVE_REINSTALL) != 0;
    if (!s.cset->changes.push(c)) {
        fail(s);
        return;
    }
    if (npkg == NONE)
        s.cset->removals++;
    else if (opkg == NONE)
        s.cset->installs++;
    else if (npkg != opkg || c.reinstall)
        s.cset->adjusts++;
}

void cset_gen_name_change(Solver &s, u32 name);
void cset_gen_name_remove(Solver &s, u32 pkg);
void cset_check_removal_by_deps(Solver &s, u32 pkg);

// Each installed package counts one against every installed name it needs.
void cset_track_deps_added(Solver &s, u32 pkg)
{
    for (const Dep &d : s.pkg(pkg).depends) {
        if (d.mask & VER_CONFLICT)
            continue;
        u32 *at = s.index.find(d.name);
        if (!at || s.nm(*at).installed_name == NONE)
            continue;
        s.nm(s.nm(*at).installed_name).requirers++;
    }
}

void cset_track_deps_removed(Solver &s, u32 pkg)
{
    for (usize k = 0; k < s.pkg(pkg).depends.size(); k++) {
        Dep d = s.pkg(pkg).depends[k];
        if (d.mask & VER_CONFLICT)
            continue;
        u32 *at = s.index.find(d.name);
        if (!at || s.nm(*at).installed_name == NONE)
            continue;
        u32 owner = s.nm(*at).installed_name;
        if (--s.nm(owner).requirers > 0)
            continue;
        if (s.nm(*at).installed_pkg != NONE)
            cset_gen_name_remove(s, s.nm(*at).installed_pkg);
    }
}

// An orphan may have no requirers only because another provider now satisfies
// it, so nothing is removed while the name still resolves.
void cset_check_removal_by_deps(Solver &s, u32 pkg)
{
    u32 n = s.pkg(pkg).name;
    if (s.nm(n).requirers == 0 && s.nm(n).chosen.pkg == NONE)
        cset_gen_name_remove(s, pkg);
}

void cset_check_install_by_iif(Solver &s, u32 name)
{
    u32 pkg = s.nm(name).chosen.pkg;
    if (pkg == NONE || !s.nm(name).seen || s.nm(name).changeset_processed)
        return;
    for (const Dep &d : s.pkg(pkg).install_if) {
        u32 *at = s.index.find(d.name);
        if (!at)
            return;
        if (!(d.mask & VER_CONFLICT) && !s.nm(*at).changeset_processed)
            return;
        if (!dep_provided(pkg, d, s.nm(*at).chosen))
            return;
    }
    cset_gen_name_change(s, name);
}

void cset_check_removal_by_iif(Solver &s, u32 name)
{
    u32 pkg = s.nm(name).installed_pkg;
    if (pkg == NONE || s.nm(name).chosen.pkg != NONE)
        return;
    if (s.nm(name).changeset_processed || s.nm(name).changeset_removed)
        return;
    for (const Dep &d : s.pkg(pkg).install_if) {
        u32 *at = s.index.find(d.name);
        if (at && s.nm(*at).changeset_removed && s.nm(*at).chosen.pkg == NONE) {
            cset_check_removal_by_deps(s, pkg);
            return;
        }
    }
}

void cset_check_by_reverse_iif(Solver &s, u32 pkg, void (*cb)(Solver &, u32))
{
    if (pkg == NONE)
        return;
    for (usize k = 0; k < s.nm(s.pkg(pkg).name).rinstall_if.size(); k++)
        cb(s, s.nm(s.pkg(pkg).name).rinstall_if[k]);
    for (usize k = 0; k < s.pkg(pkg).provides.size(); k++) {
        Dep d = s.pkg(pkg).provides[k];
        if (u32 *at = s.index.find(d.name))
            for (usize j = 0; j < s.nm(*at).rinstall_if.size(); j++)
                cb(s, s.nm(*at).rinstall_if[j]);
    }
}

// Whatever holds this name now goes first, so a replacement never lands on an
// occupied name.
void cset_gen_name_preprocess(Solver &s, u32 name)
{
    if (s.nm(name).changeset_processed)
        return;
    s.nm(name).changeset_processed = true;

    u32 held = s.nm(name).installed_pkg;
    u32 pick = s.nm(name).chosen.pkg;
    if (held != NONE && (pick == NONE || s.pkg(pick).name != name))
        cset_gen_name_remove(s, held);

    for (usize k = 0; k < s.nm(name).providers.size(); k++) {
        u32 p0 = s.nm(name).providers[k].pkg;
        u32 n0 = s.pkg(p0).name;
        if (s.nm(n0).installed_pkg == p0 && s.nm(n0).chosen.pkg == NONE)
            cset_gen_name_remove(s, p0);
    }
}

// apk_dep_analyze, restricted to the one answer the removal walk wants.
bool dep_satisfies_pkg(Solver &s, u32 dpkg, const Dep &d, u32 pkg)
{
    if (d.name == s.pkg(pkg).stanza->name)
        return !(d.mask & VER_CONFLICT) && dep_satisfied(d, s.pkg(pkg).stanza->version);
    for (const Dep &q : s.pkg(pkg).provides)
        if (q.name == d.name)
            return dep_provided(dpkg, d, SolveProvider{ pkg, q.version });
    return false;
}

// Everything installed that needs this goes before it does.
void cset_gen_name_remove(Solver &s, u32 pkg)
{
    u32 name = s.pkg(pkg).name;
    if (s.pkg(pkg).in_changeset ||
        (s.nm(name).chosen.pkg != NONE && s.pkg(s.nm(name).chosen.pkg).name == name))
        return;

    s.nm(name).changeset_removed = true;
    s.pkg(pkg).in_changeset      = true;

    u32 walk = ++s.order;
    for (u32 pass = 0; pass < 1 + s.pkg(pkg).provides.size(); pass++) {
        u32 n0;
        if (pass == 0)
            n0 = name;
        else {
            u32 *at = s.index.find(s.pkg(pkg).provides[pass - 1].name);
            if (!at)
                continue;
            n0 = *at;
        }
        for (usize k = 0; k < s.nm(n0).rdepends.size(); k++) {
            u32 rn = s.nm(n0).rdepends[k];
            for (usize j = 0; j < s.nm(rn).providers.size(); j++) {
                u32 p0 = s.nm(rn).providers[j].pkg;
                if (!s.pkg(p0).ipkg || s.pkg(p0).walk == walk)
                    continue;
                s.pkg(p0).walk = walk;
                for (const Dep &d : s.pkg(p0).depends) {
                    if (dep_satisfies_pkg(s, p0, d, pkg)) {
                        cset_gen_name_remove(s, p0);
                        break;
                    }
                }
            }
        }
    }

    cset_check_by_reverse_iif(s, pkg, cset_check_removal_by_iif);
    record_change(s, pkg, NONE);
    cset_track_deps_removed(s, pkg);
}

void cset_gen_dep(Solver &s, u32 ppkg, const Dep &d)
{
    u32 *at = s.index.find(d.name);
    if (!at) {
        if (!(d.mask & VER_CONFLICT))
            mark_error(s, ppkg);
        return;
    }
    u32 name = *at;
    u32 pkg  = s.nm(name).chosen.pkg;

    if (!dep_provided(ppkg, d, s.nm(name).chosen))
        mark_error(s, ppkg);
    cset_gen_name_change(s, name);
    if (pkg != NONE && s.pkg(pkg).error)
        mark_error(s, ppkg);
}

void cset_gen_name_change(Solver &s, u32 name)
{
    if (s.nm(name).changeset_processed)
        return;
    cset_gen_name_preprocess(s, name);

    u32 pkg = s.nm(name).chosen.pkg;
    if (pkg == NONE || s.pkg(pkg).in_changeset)
        return;

    s.pkg(pkg).in_changeset = true;
    cset_gen_name_preprocess(s, s.pkg(pkg).name);
    for (usize k = 0; k < s.pkg(pkg).provides.size(); k++)
        if (u32 *at = s.index.find(s.pkg(pkg).provides[k].name))
            cset_gen_name_preprocess(s, *at);

    u32 opkg = s.nm(s.pkg(pkg).name).installed_pkg;
    cset_check_by_reverse_iif(s, opkg, cset_check_removal_by_iif);

    for (usize k = 0; k < s.pkg(pkg).depends.size(); k++)
        cset_gen_dep(s, pkg, s.pkg(pkg).depends[k]);

    record_change(s, opkg, pkg);

    cset_check_by_reverse_iif(s, pkg, cset_check_install_by_iif);
    cset_track_deps_added(s, pkg);
    if (opkg != NONE)
        cset_track_deps_removed(s, opkg);
}

void generate_changeset(Solver &s, Span<const Dep> world)
{
    for (SolveName &n : s.names) {
        n.installed_pkg  = NONE;
        n.installed_name = NONE;
        n.requirers      = 0;
    }
    for (u32 i = 0; i < s.pkgs.size(); i++) {
        if (!s.pkg(i).ipkg)
            continue;
        s.nm(s.pkg(i).name).installed_pkg  = i;
        s.nm(s.pkg(i).name).installed_name = s.pkg(i).name;
        for (const Dep &d : s.pkg(i).provides)
            if (!d.version.empty())
                if (u32 *at = s.index.find(d.name))
                    s.nm(*at).installed_name = s.pkg(i).name;
    }
    for (u32 i = 0; i < s.pkgs.size(); i++)
        if (s.pkg(i).ipkg)
            cset_track_deps_added(s, i);
    for (u32 i = 0; i < s.pkgs.size(); i++)
        if (s.pkg(i).ipkg)
            cset_check_removal_by_deps(s, i);

    for (const Dep &d : world)
        cset_gen_dep(s, NONE, d);
    for (u32 i = 0; i < s.pkgs.size(); i++)
        if (s.pkg(i).ipkg)
            cset_gen_name_change(s, s.pkg(i).name);
}

// ---------------------------------------------------------------- the report

// apk's reporter, reduced to its labels. It re-derives what breaks from the
// graph rather than from `conflicts`: the solver stops applying a constraint
// once the name behind it has no options left, so the counter under-reports.
bool dep_breaks_pkg(Solver &s, u32 dpkg, const Dep &d, u32 pkg)
{
    if (d.name == s.pkg(pkg).stanza->name)
        return !dep_satisfied(d, s.pkg(pkg).stanza->version);
    for (const Dep &q : s.pkg(pkg).provides)
        if (q.name == d.name)
            return !dep_provided(dpkg, d, SolveProvider{ pkg, q.version });
    return false; // irrelevant: this dependency is not about this package
}

// Whether world, or anything else chosen, states a constraint this package
// fails — apk's `breaks:` field, and the whole of why a label is printed.
bool breaks_something(Solver &s, u32 pkg, Span<const Dep> world)
{
    for (const Dep &d : world)
        if (dep_breaks_pkg(s, NONE, d, pkg))
            return true;
    for (u32 i = 0; i < s.pkgs.size(); i++) {
        if (!s.pkg(i).in_changeset)
            continue;
        for (const Dep &d : s.pkg(i).depends)
            if (dep_breaks_pkg(s, i, d, pkg))
                return true;
    }
    return false;
}

// A name nothing chose, reported once.
bool report_dep(Solver &s, u32 ppkg, const Dep &d, Vec<Str> &done)
{
    if (d.mask & VER_CONFLICT)
        return true;
    u32 *at = s.index.find(d.name);
    if (at && (s.nm(*at).chosen.pkg != NONE || dep_provided(ppkg, d, s.nm(*at).chosen)))
        return true;
    for (Str seen : done)
        if (seen == d.name)
            return true;
    if (!done.push(d.name))
        return fail(s);

    SolveFail f;
    f.name = d.name;
    f.kind = (!at || s.nm(*at).providers.empty()) ? SolveFailKind::Missing : SolveFailKind::Virtual;
    return s.cset->errors.push(f) || fail(s);
}

bool collect_errors(Solver &s, Span<const Dep> world)
{
    Vec<Str> done;
    for (const Dep &d : world)
        if (!report_dep(s, NONE, d, done))
            return false;

    for (u32 i = 0; i < s.pkgs.size(); i++)
        s.pkg(i).walk = 0;
    for (const SolveChange &c : s.cset->changes) {
        if (!c.new_pkg)
            continue;
        for (u32 i = 0; i < s.pkgs.size(); i++) {
            if (s.pkg(i).stanza != c.new_pkg || s.pkg(i).walk)
                continue;
            s.pkg(i).walk = 1;
            if (s.pkg(i).direct_error || breaks_something(s, i, world)) {
                SolveFail f;
                f.kind    = SolveFailKind::Package;
                f.name    = c.new_pkg->name;
                f.version = c.new_pkg->version;
                if (!s.cset->errors.push(f))
                    return fail(s);
            }
            for (const Dep &d : s.pkg(i).depends)
                if (!report_dep(s, i, d, done))
                    return false;
            break;
        }
    }
    return true;
}

} // namespace

Result<void> solve(const SolveInput &in, Changeset &out)
{
    Solver s;
    s.cset    = &out;
    s.flags   = in.flags;
    s.inherit = in.flags;

    // Identity is the digest, as apk_db_pkg_add's table is: two stanzas that
    // share one is one package, and the second name gains no provider. That
    // holds within the index too, not only across it and the installed set.
    HashMap<Str, u32> by_digest;
    for (const PackageStanza &st : in.repo) {
        Str key = Str(reinterpret_cast<const char *>(st.digest), SHA256_SIZE);
        if (by_digest.find(key))
            continue;
        if (!add_package(s, &st, false))
            return Err(Error::NoMemory);
        if (!by_digest.insert(key, u32(s.pkgs.size() - 1)))
            return Err(Error::NoMemory);
    }
    for (const PackageStanza &st : in.installed) {
        Str key = Str(reinterpret_cast<const char *>(st.digest), SHA256_SIZE);
        if (u32 *at = by_digest.find(key)) {
            s.pkgs[*at].ipkg = true;
            continue;
        }
        if (!add_package(s, &st, true))
            return Err(Error::NoMemory);
        if (!by_digest.insert(key, u32(s.pkgs.size() - 1)))
            return Err(Error::NoMemory);
    }
    if (!build_reverse(s))
        return Err(Error::NoMemory);

    for (const SolveRequest &r : in.named) {
        u32 n = intern(s, r.name);
        if (n == NONE)
            return Err(Error::NoMemory);
        s.nm(n).requested = true;
        for (usize k = 0; k < s.nm(n).providers.size(); k++) {
            s.pkg(s.nm(n).providers[k].pkg).flags |= r.flags;
            s.pkg(s.nm(n).providers[k].pkg).inheritable |= r.inherit;
        }
    }

    Vec<Dep> world;
    for (const Dep &d : in.world) {
        usize at = world.size();
        while (at > 0 && name_cmp(world[at - 1].name, d.name) > 0)
            at--;
        if (!world.insert(at, d))
            return Err(Error::NoMemory);
    }

    for (const Dep &d : world) {
        u32 n = intern(s, d.name);
        if (n == NONE)
            return Err(Error::NoMemory);
        discover_name(s, n);
    }
    for (const Dep &d : world)
        apply_constraint(s, NONE, d);
    s.inherit = 0;

    for (;;) {
        while (s.dirty_head != NONE) {
            u32 i        = s.dirty_head;
            s.dirty_head = s.nm(i).dirty_next;
            if (s.dirty_head == NONE)
                s.dirty_tail = NONE;
            s.nm(i).dirty_queued = false;
            reconsider_name(s, i);
        }
        u32 i = dequeue_next_name(s);
        if (i == NONE)
            break;
        select_package(s, i);
    }

    generate_changeset(s, world);
    if (!collect_errors(s, world))
        return Err(Error::NoMemory);
    return s.oom ? Err(Error::NoMemory) : Result<void>();
}

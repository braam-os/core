#include "plan.h"

#include "version.h"

namespace {

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

Str plan_verb(const SolveChange &c)
{
    if (!c.old_pkg)
        return "Installing";
    if (!c.new_pkg)
        return "Purging";
    if (c.old_pkg == c.new_pkg)
        return c.reinstall ? "Reinstalling" : Str();
    switch (version_compare(c.new_pkg->version, c.old_pkg->version)) {
    case VER_LESS:
        return "Downgrading";
    case VER_GREATER:
        return "Upgrading";
    default:
        return "Replacing";
    }
}

bool plan_changes(const Changeset &cs, String &out)
{
    for (const SolveChange &c : cs.changes) {
        Str verb = plan_verb(c);
        if (verb.empty())
            continue;

        const PackageStanza *p = c.new_pkg ? c.new_pkg : c.old_pkg;
        if (!out.append(verb) || !out.push(' ') || !out.append(p->name) || !out.append(" ("))
            return false;
        if (c.old_pkg && c.new_pkg && c.old_pkg != c.new_pkg)
            if (!out.append(c.old_pkg->version) || !out.append(" -> "))
                return false;
        if (!out.append(p->version) || !out.append(")\n"))
            return false;
    }
    return true;
}

PlanScripts plan_scripts(const SolveChange &c)
{
    PlanScripts s;
    if (plan_verb(c).empty())
        return s;

    if (!c.old_pkg) {
        s.before = ZipMeta::PreInstall;
        s.after  = ZipMeta::PostInstall;
        s.pkg    = c.new_pkg;
    } else if (!c.new_pkg) {
        // The version leaving, out of the store directory it still has.
        s.before = ZipMeta::PreDeinstall;
        s.after  = ZipMeta::PostDeinstall;
        s.pkg    = c.old_pkg;
    } else {
        // Any version change, down as well as up: apk's pair either way.
        s.before      = ZipMeta::PreUpgrade;
        s.after       = ZipMeta::PostUpgrade;
        s.pkg         = c.new_pkg;
        s.old_version = c.old_pkg->version;
    }
    return s;
}

bool plan_errors(const Changeset &cs, Str lead, String &out)
{
    if (!out.append(lead) || !out.push('\n'))
        return false;
    for (const SolveFail &f : cs.errors) {
        if (!out.append("  ") || !out.append(f.name))
            return false;
        bool ok = f.kind == SolveFailKind::Package
                      ? out.push('-') && out.append(f.version)
                      : out.append(f.kind == SolveFailKind::Virtual ? " (virtual)"
                                                                    : " (no such package)");
        if (!ok || !out.push('\n'))
            return false;
    }
    return true;
}

bool plan_installed(const Changeset &cs, Vec<Installed> &out)
{
    // An insertion sort: a generation is a handful of names.
    for (const SolveChange &c : cs.changes) {
        if (!c.new_pkg)
            continue;
        Installed p{ c.new_pkg->name, c.new_pkg->version };
        usize at = out.size();
        while (at > 0 && before(p.name, out[at - 1].name))
            at--;
        if (!out.insert(at, p))
            return false;
    }
    return true;
}

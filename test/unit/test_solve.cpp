#include "cmd/pkg/solve.h"
#include "cmd/pkg/stanza.h"
#include "harness.h"
#include "kernel/fmt.h"
#include "kernel/hash.h"
#include "kernel/string.h"
#include "kernel/traits.h"

namespace {

// apk-tools/test/solver/, ported by tools/mkfixtures.py: @FILE names each,
// a .repo or .installed verbatim and a .test with @EXPECT reduced to the
// actions apk chose.
constexpr Str DATA =
#include "solve.data"
    ;

constexpr usize CASES = 72;

Str file_of(Str name)
{
    Buf<64> want;
    want.put("\n@FILE ").put(name).put('\n');
    usize at = DATA.find(want.str());
    if (at == Str::npos)
        return Str();
    Str rest  = DATA.substr(at + want.str().size());
    usize end = rest.find("\n@FILE ");
    return end == Str::npos ? rest : rest.substr(0, end + 1);
}

bool line_of(Str &rest, Str &line)
{
    if (rest.empty())
        return false;
    usize at = rest.find('\n');
    line     = at == Str::npos ? rest : rest.substr(0, at);
    rest     = at == Str::npos ? Str() : rest.substr(at + 1);
    return true;
}

Str word_of(Str &rest)
{
    while (!rest.empty() && rest[0] == ' ')
        rest = rest.substr(1);
    usize at = 0;
    while (at < rest.size() && rest[at] != ' ')
        at++;
    Str w = rest.substr(0, at);
    rest  = rest.substr(at);
    return w;
}

// A .repo or .installed, which is §3.2's grammar and so already readable.
bool stanzas_of(Str text, Vec<PackageStanza> &out)
{
    StanzaReader r(text, STANZA_PACKAGE);
    Vec<StanzaField> f;
    for (;;) {
        StanzaRead got = r.next(f);
        if (got == StanzaRead::End)
            return true;
        if (got == StanzaRead::Malformed)
            return false;
        PackageStanza p;
        if (got == StanzaRead::Unusable || package_read(f, p) != StanzaRead::Ok)
            continue; // an unknown uppercase letter drops its record, not the file
        if (!out.push(move(p)))
            return false;
    }
}

// One case, as solver.sh assembles it.
struct Case {
    Vec<PackageStanza> repo, installed;
    Vec<Dep> world;
    Vec<SolveRequest> named;
    u32 flags = 0;
    Str expect;
    bool runnable = false;
};

bool push_world(Case &c, Str spec)
{
    Dep d;
    if (dep_parse(spec, d) == DepParse::Malformed)
        return true;
    for (Dep &had : c.world)
        if (had.name == d.name) {
            had = d;
            return true;
        }
    return c.world.push(d);
}

void drop_world(Case &c, Str name)
{
    for (usize i = 0; i < c.world.size(); i++)
        if (c.world[i].name == name) {
            c.world.erase(i);
            return;
        }
}

bool name_flags(Case &c, Str name, u32 flags, u32 inherit)
{
    for (SolveRequest &r : c.named)
        if (r.name == name) {
            r.flags |= flags;
            r.inherit |= inherit;
            return true;
        }
    return c.named.push(SolveRequest{ name, flags, inherit });
}

// Whether the index offers this name at all, under P: or under p:.
bool provided_by_repo(const Case &c, Str name)
{
    for (const PackageStanza &p : c.repo) {
        if (p.name == name)
            return true;
        Str rest = p.provides, spec;
        while (dep_next(rest, spec)) {
            Dep d;
            if (dep_parse(spec, d) != DepParse::Malformed && d.name == name)
                return true;
        }
    }
    return false;
}

// @ARGS, as app_add.c, app_del.c and app_upgrade.c set it up: `add` and `del`
// pass no run-wide flag and name their targets, `upgrade` does the reverse.
bool apply_args(Case &c, Str args)
{
    Str rest = args;
    Str verb = word_of(rest);
    bool del = verb == "del", add = verb == "add";
    bool prune = false, ignore = false;
    u32 flags = 0;

    Vec<Str> operands;
    for (;;) {
        Str w = word_of(rest);
        if (w.empty())
            break;
        if (w == "-a" || w == "--available")
            flags |= SOLVE_AVAILABLE;
        else if (w == "--latest" || w == "-l")
            flags |= SOLVE_LATEST;
        else if (w == "--upgrade" || w == "-u")
            flags |= SOLVE_UPGRADE;
        else if (w == "--prune")
            prune = true;
        else if (w == "--ignore")
            ignore = true;
        else if (!operands.push(w))
            return false;
    }

    if (add) {
        for (Str w : operands) {
            Dep d;
            if (dep_parse(w, d) == DepParse::Malformed)
                continue;
            if (!push_world(c, w) || !name_flags(c, d.name, flags, flags))
                return false;
        }
    } else if (del) {
        for (Str w : operands) {
            Dep d;
            if (dep_parse(w, d) == DepParse::Malformed)
                continue;
            drop_world(c, d.name);
            if (!name_flags(c, d.name, SOLVE_REMOVE, 0))
                return false;
        }
    } else {
        c.flags = SOLVE_UPGRADE | flags;
        if (!operands.empty()) {
            // Named packages are the upgrade; world is not.
            if (!ignore)
                c.flags &= ~SOLVE_UPGRADE;
            for (Str w : operands)
                if (!name_flags(c, w, ignore ? SOLVE_INSTALLED : SOLVE_UPGRADE, 0))
                    return false;
        }
        // --prune: a world member no repository answers for stops being wanted.
        if (prune)
            for (usize i = c.world.size(); i > 0; i--)
                if (!provided_by_repo(c, c.world[i - 1].name))
                    c.world.erase(i - 1);
    }
    return true;
}

bool load_case(Str text, Case &c)
{
    Str rest        = text, line, args;
    bool in_expect  = false;
    usize expect_at = 0, seen = 0;

    while (line_of(rest, line)) {
        seen += line.size() + 1;
        if (in_expect)
            continue;
        if (line == "@EXPECT") {
            in_expect = true;
            expect_at = seen;
            continue;
        }
        Str body = line;
        Str tag  = word_of(body);
        if (tag == "@ARGS") {
            args       = body;
            c.runnable = true;
        } else if (tag == "@REPO") {
            if (!stanzas_of(file_of(word_of(body)), c.repo))
                return false;
        } else if (tag == "@INSTALLED") {
            if (!stanzas_of(file_of(word_of(body)), c.installed))
                return false;
        } else if (tag == "@WORLD") {
            for (;;) {
                Str w = word_of(body);
                if (w.empty())
                    break;
                if (!push_world(c, w))
                    return false;
            }
        }
    }
    c.expect = in_expect ? text.substr(expect_at) : Str();
    return c.runnable && apply_args(c, args);
}

// ------------------------------------------------------------ the rendering

// apk's print_change verbs, without its counter.
Str verb_of(const SolveChange &c)
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

// apk prints the changeset or the errors, never both: a failed solve goes to
// the reporter instead of to the commit.
bool render(const Changeset &cs, String &out)
{
    if (!cs.errors.empty()) {
        if (!out.append("ERROR:\n"))
            return false;
        for (const SolveFail &f : cs.errors) {
            if (!out.append("  ") || !out.append(f.name))
                return false;
            bool ok = f.kind == SolveFailKind::Package
                          ? out.push('-') && out.append(f.version)
                          : out.append(f.kind == SolveFailKind::Virtual ? " (virtual)"
                                                                        : " (no such package)");
            if (!ok || !out.append(":\n"))
                return false;
        }
        return true;
    }

    for (const SolveChange &c : cs.changes) {
        Str verb = verb_of(c);
        if (verb.empty())
            continue;
        const PackageStanza *p = c.new_pkg ? c.new_pkg : c.old_pkg;
        if (!out.append(verb) || !out.push(' ') || !out.append(p->name) || !out.append(" ("))
            return false;
        if (c.old_pkg && c.new_pkg && c.old_pkg != c.new_pkg) {
            if (!out.append(c.old_pkg->version) || !out.append(" -> "))
                return false;
        }
        if (!out.append(p->version) || !out.append(")\n"))
            return false;
    }
    return true;
}

void run_case(Str name)
{
    Str text = file_of(name);
    if (text.empty()) {
        test_check(false, name, __FILE_NAME__, __LINE__);
        return;
    }

    Case c;
    if (!load_case(text, c)) {
        test_check(false, name, __FILE_NAME__, __LINE__);
        return;
    }

    SolveInput in;
    in.repo      = c.repo;
    in.installed = c.installed;
    in.world     = c.world;
    in.named     = c.named;
    in.flags     = c.flags;

    Changeset cs;
    String got;
    bool ok = solve(in, cs).is_ok() && render(cs, got);
    if (ok && got.str() == c.expect)
        return;
    test_check(false, name, __FILE_NAME__, __LINE__);

    // A fixture that fails is worth reading, so it says what it wanted.
    Str rest = c.expect, line;
    while (line_of(rest, line)) {
        Buf<128> b;
        b.put("  want| ").put(line);
        host_log(b.str().data(), b.str().size());
    }
    rest = got.str();
    while (line_of(rest, line)) {
        Buf<128> b;
        b.put("  got | ").put(line);
        host_log(b.str().data(), b.str().size());
    }
}

constexpr Str NAMES[] = {
    "basic1.test",         "basic13.test",          "basic14.test",        "basic15.test",
    "basic17.test",        "basic18.test",          "basic2.test",         "basic21.test",
    "basic3.test",         "basic4.test",           "basic5.test",         "basic6.test",
    "complicated1.test",   "complicated2.test",     "complicated3.test",   "complicated4.test",
    "conflict1.test",      "conflict2.test",        "conflict3.test",      "error1.test",
    "error2.test",         "error3.test",           "error4.test",         "error5.test",
    "fuzzy1.test",         "fuzzy2.test",           "fuzzy3.test",         "installif1.test",
    "installif10.test",    "installif13.test",      "installif2.test",     "installif3.test",
    "installif4.test",     "installif5.test",       "installif6.test",     "installif8.test",
    "installif9.test",     "provides-prio1.test",   "provides-prio2.test", "provides-prio4.test",
    "provides-prio5.test", "provides-prio6.test",   "provides-swap.test",  "provides-swap2.test",
    "provides-swap3.test", "provides-upgrade.test", "provides1.test",      "provides10.test",
    "provides11.test",     "provides12.test",       "provides13.test",     "provides14.test",
    "provides15.test",     "provides16.test",       "provides17.test",     "provides18.test",
    "provides19.test",     "provides2.test",        "provides20.test",     "provides21.test",
    "provides22.test",     "provides3.test",        "provides4.test",      "provides5.test",
    "provides6.test",      "provides7.test",        "provides8.test",      "provides9.test",
    "upgrade1.test",       "upgrade2.test",         "upgrade3.test",       "upgrade4.test",
};

} // namespace

void test_solve()
{
    test_begin("solve");

    // A case that quietly went missing is this file's failure mode.
    CHECK_EQ(sizeof(NAMES) / sizeof(NAMES[0]), CASES);
    for (Str name : NAMES)
        run_case(name);
}

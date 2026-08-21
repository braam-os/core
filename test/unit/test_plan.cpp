#include "cmd/pkg/plan.h"
#include "harness.h"

namespace {

PackageStanza pkg(Str name, Str version)
{
    PackageStanza p;
    p.name    = name;
    p.version = version;
    return p;
}

} // namespace

void test_plan()
{
    test_begin("plan");

    const PackageStanza old_app = pkg("app", "1"), new_app = pkg("app", "2");
    const PackageStanza libz = pkg("libz", "1.0-r0"), gone = pkg("libold", "1");
    const PackageStanza keep = pkg("less", "1.6-r1");
    const PackageStanza swap = pkg("app", "2"); // another provider, one version

    // apk's six verbs, and the seventh case that is not one.
    CHECK(plan_verb(SolveChange{ nullptr, &libz, false }) == "Installing");
    CHECK(plan_verb(SolveChange{ &gone, nullptr, false }) == "Purging");
    CHECK(plan_verb(SolveChange{ &libz, &libz, false }).empty());
    CHECK(plan_verb(SolveChange{ &libz, &libz, true }) == "Reinstalling");
    CHECK(plan_verb(SolveChange{ &old_app, &new_app, false }) == "Upgrading");
    CHECK(plan_verb(SolveChange{ &new_app, &old_app, false }) == "Downgrading");
    CHECK(plan_verb(SolveChange{ &new_app, &swap, false }) == "Replacing");

    Changeset cs;
    CHECK(cs.changes.push(SolveChange{ nullptr, &libz, false }));
    CHECK(cs.changes.push(SolveChange{ &old_app, &new_app, false }));
    CHECK(cs.changes.push(SolveChange{ &gone, nullptr, false }));
    CHECK(cs.changes.push(SolveChange{ &keep, &keep, false }));

    // A change with no verb prints nothing, and an upgrade prints both sides.
    {
        String out;
        CHECK(plan_changes(cs, out));
        CHECK(out.str() ==
              "Installing libz (1.0-r0)\n"
              "Upgrading app (1 -> 2)\n"
              "Purging libold (1)\n");
    }

    // The set that is left: a removal is not in it, a change that changes
    // nothing is, and it is sorted, because the link farm follows this order.
    {
        Vec<Installed> want;
        CHECK(plan_installed(cs, want));
        CHECK_EQ(want.size(), 3);
        CHECK(want[0].name == "app" && want[0].version == "2");
        CHECK(want[1].name == "less");
        CHECK(want[2].name == "libz");
    }

    // apk's three labels, under a lead of the caller's.
    {
        Changeset bad;
        CHECK(bad.errors.push(SolveFail{ SolveFailKind::Missing, "cmd:sh", "" }));
        CHECK(bad.errors.push(SolveFail{ SolveFailKind::Virtual, "libc", "" }));
        CHECK(bad.errors.push(SolveFail{ SolveFailKind::Package, "d", "2.0" }));

        String out;
        CHECK(plan_errors(bad, "pkg: cannot install:", out));
        CHECK(out.str() ==
              "pkg: cannot install:\n"
              "  cmd:sh (no such package)\n"
              "  libc (virtual)\n"
              "  d-2.0\n");
    }
}

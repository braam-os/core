#include "cmd/pkg/db.h"
#include "cmd/pkg/index.h"
#include "cmd/pkg/trust.h"
#include "fakehost.h"
#include "harness.h"
#include "kernel/fmt.h"
#include "kernel/hostcall.h"
#include "kernel/sched.h"
#include "kernel/traits.h"

namespace {

// An anchor and the indexes under it, over throwaway keys generated once out
// of tree. Blocks are named by an @line: an empty line is part of the grammar.
constexpr Str DATA =
#include "index.data"
    ;

constexpr Str REPO = "https://packages.example/braam";
constexpr Str URL  = "https://packages.example/braam/index";

// Before every fixture's E but one.
constexpr u64 NOW = 1755648000000ull;

Str fixture(Str name)
{
    Buf<32> want;
    want.put("\n@").put(name).put('\n');
    usize at = DATA.find(want.str());
    if (at == Str::npos)
        return Str();
    Str rest  = DATA.substr(at + want.str().size());
    usize end = rest.find("\n@");
    return end == Str::npos ? rest : rest.substr(0, end + 1);
}

// A pointer, because a namespace-scope global must be trivially destructible.
FakeHost *host;
bool answered;
IndexStep step;
Error failure;

Task<i32> ask_check(CheckedIndex *out)
{
    Task<Result<void>> t = index_check(*host, REPO, *out, step);
    if (!t)
        co_return 1;
    Result<void> r = co_await t;
    answered       = true;
    if (r.is_err()) {
        failure = r.error();
        co_return 1;
    }
    failure = Error::Closed; // a sentinel no step reports
    co_return 0;
}

// True when the index was accepted; `step` and `failure` say why not.
bool run_check(CheckedIndex &out)
{
    sched_reset();
    answered = false;
    step     = IndexStep::Read;
    failure  = Error::Invalid;
    if (sched_spawn(ask_check(&out)) == 0)
        return false;
    sched_tick(0);
    return answered && failure == Error::Closed;
}

// A fresh host at the fixed time, with the good anchor and a floor of 40.
bool ready(FakeHost &h, Str index)
{
    h.clock = NOW;
    return h.file(ANCHOR_PATH, fixture("anchor")) && h.file(PKG_INDEX, fixture("stored")) &&
           h.route(URL, index);
}

} // namespace

void test_index()
{
    test_begin("index");

    // The happy path: §7's seven steps, and the two packages behind them.
    {
        FakeHost h;
        host = &h;
        CHECK(ready(h, fixture("good")));
        CheckedIndex c;
        CHECK(run_check(c));
        CHECK_EQ(u32(c.head.version), 41);
        CHECK(c.head.url == REPO);
        CHECK(!c.unchanged);
        CHECK_EQ(c.now, NOW);
        CHECK_EQ(c.packages.size(), 2);
        CHECK(index_find(c, "awk") != nullptr);
        CHECK(index_find(c, "less") != nullptr);
        // A name the index does not list does not exist (§7 step 7).
        CHECK(index_find(c, "nawk") == nullptr);
        CHECK_EQ(h.opened, 1);
        CHECK_EQ(h.closed, 1); // and the body is closed either way
    }

    // Step 2: no anchor at all, and one that does not meet its threshold.
    {
        FakeHost h;
        host    = &h;
        h.clock = NOW;
        CHECK(h.route(URL, fixture("good")));
        CheckedIndex c;
        CHECK(!run_check(c));
        CHECK(step == IndexStep::Anchor);
        CHECK(failure == Error::NotFound);
    }
    {
        FakeHost h;
        host    = &h;
        h.clock = NOW;
        CHECK(h.file(ANCHOR_PATH, fixture("good"))); // not an anchor at all
        CHECK(h.route(URL, fixture("good")));
        CheckedIndex c;
        CHECK(!run_check(c));
        CHECK(step == IndexStep::Anchor);
        CHECK(failure == Error::Invalid);
    }

    // Step 3, the cap: refused past INDEX_MAX, and not one byte short of it.
    // The body never reaches a signature, so its bytes need not be signed.
    {
        String over;
        CHECK(over.append("Y:x\n\n"));
        while (over.size() <= INDEX_MAX)
            CHECK(over.push('x'));

        FakeHost h;
        host    = &h;
        h.clock = NOW;
        CHECK(h.file(ANCHOR_PATH, fixture("anchor")));
        CHECK(h.route(URL, over.str()));
        CheckedIndex c;
        CHECK(!run_check(c));
        CHECK(step == IndexStep::Fetch);
        CHECK_EQ(h.closed, 1); // abandoned, and still closed

        // Exactly the cap passes the fetch and stops at the signatures.
        String at;
        CHECK(at.append(over.str().substr(0, INDEX_MAX)));
        FakeHost g;
        host    = &g;
        g.clock = NOW;
        CHECK(g.file(ANCHOR_PATH, fixture("anchor")));
        CHECK(g.route(URL, at.str()));
        CheckedIndex d;
        CHECK(!run_check(d));
        CHECK(step == IndexStep::Signature);
    }

    // A fetch that answered something other than 200.
    {
        FakeHost h;
        host    = &h;
        h.clock = NOW;
        CHECK(h.file(ANCHOR_PATH, fixture("anchor")));
        CHECK(h.route(URL, fixture("good"), 404));
        CheckedIndex c;
        CHECK(!run_check(c));
        CHECK(step == IndexStep::Fetch);
        CHECK(failure == Error::NotFound);
    }

    // Step 4: a body changed after signing, and a signature by a key the
    // anchor does not name under `index`.
    {
        FakeHost h;
        host = &h;
        CHECK(ready(h, fixture("tampered")));
        CheckedIndex c;
        CHECK(!run_check(c));
        CHECK(step == IndexStep::Signature);
        CHECK(failure == Error::Perm);
    }
    {
        FakeHost h;
        host = &h;
        CHECK(ready(h, fixture("stranger")));
        CheckedIndex c;
        CHECK(!run_check(c));
        CHECK(step == IndexStep::Signature);
    }

    // §3.1: a higher X refuses the file, and N must be the repository asked
    // for. Both are signed, so only the header can refuse them.
    {
        FakeHost h;
        host = &h;
        CHECK(ready(h, fixture("grammar")));
        CheckedIndex c;
        CHECK(!run_check(c));
        CHECK(step == IndexStep::Header);
        CHECK(failure == Error::Unsupported);
    }
    {
        FakeHost h;
        host = &h;
        CHECK(ready(h, fixture("foreign")));
        CheckedIndex c;
        CHECK(!run_check(c));
        CHECK(step == IndexStep::Header);
        CHECK(failure == Error::Perm);
    }

    // Step 5: lower is a rollback, equal is nothing to do, and a stored index
    // that does not read refuses rather than becoming a floor of zero.
    {
        FakeHost h;
        host = &h;
        CHECK(ready(h, fixture("older")));
        CheckedIndex c;
        CHECK(!run_check(c));
        CHECK(step == IndexStep::Version);
        CHECK(failure == Error::Perm);
    }
    {
        FakeHost h;
        host = &h;
        CHECK(ready(h, fixture("same")));
        CheckedIndex c;
        CHECK(run_check(c));
        CHECK(c.unchanged);
    }
    {
        FakeHost h;
        host    = &h;
        h.clock = NOW;
        CHECK(h.file(ANCHOR_PATH, fixture("anchor")));
        CHECK(h.file(PKG_INDEX, "not an index\n"));
        CHECK(h.route(URL, fixture("good")));
        CheckedIndex c;
        CHECK(!run_check(c));
        CHECK(step == IndexStep::Version);
    }
    // No stored index is no floor, and any version passes.
    {
        FakeHost h;
        host    = &h;
        h.clock = NOW;
        CHECK(h.file(ANCHOR_PATH, fixture("anchor")));
        CHECK(h.route(URL, fixture("older")));
        CheckedIndex c;
        CHECK(run_check(c));
        CHECK_EQ(u32(c.head.version), 39);
    }

    // Step 6, against the time from step 1.
    {
        FakeHost h;
        host = &h;
        CHECK(ready(h, fixture("expired")));
        CheckedIndex c;
        CHECK(!run_check(c));
        CHECK(step == IndexStep::Expiry);
    }
    {
        FakeHost h;
        host = &h;
        CHECK(ready(h, fixture("good")));
        CheckedIndex c;
        h.clock = 2000000000000ull; // E itself, which has passed
        CHECK(!run_check(c));
        CHECK(step == IndexStep::Expiry);
    }

    // Step 7, §1's two scopes: a line that is not a field takes the file down,
    // an unusable record takes only itself.
    {
        FakeHost h;
        host = &h;
        CHECK(ready(h, fixture("malformed")));
        CheckedIndex c;
        CHECK(!run_check(c));
        CHECK(step == IndexStep::Read);
    }
    {
        FakeHost h;
        host = &h;
        CHECK(ready(h, fixture("oneunusable")));
        CheckedIndex c;
        CHECK(run_check(c));
        CHECK_EQ(c.packages.size(), 1);
        CHECK(index_find(c, "less") != nullptr);
        CHECK(index_find(c, "awk") == nullptr);
    }

    // A browser with no Ed25519 is a fault, not a bad signature.
    {
        FakeHost h;
        host = &h;
        CHECK(ready(h, fixture("good")));
        h.no_ed25519 = true;
        CheckedIndex c;
        CHECK(!run_check(c));
        CHECK(step == IndexStep::Anchor);
        CHECK(failure == Error::Unsupported);
    }

    // Every step names itself.
    CHECK(index_step_name(IndexStep::Clock) == "clock");
    CHECK(index_step_name(IndexStep::Read) == "read");

    CHECK_EQ(host_orphans(), 0);
}

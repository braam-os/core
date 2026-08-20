#include "cmd/pkg/trust.h"
#include "harness.h"
#include "kernel/fmt.h"
#include "kernel/hostcall.h"
#include "kernel/sched.h"
#include "kernel/traits.h"
#include "svc/svc.h"

namespace {

// Anchors over throwaway keys, generated once out of tree. Blocks are named by
// an @line, since an empty line is part of what is being tested.
constexpr Str DATA =
#include "anchor.data"
    ;

// 2025-08-20: before every fixture's E but one.
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

bool load(Str name, AnchorFile &a)
{
    String s;
    return s.append(fixture(name)) && anchor_file_read(move(s), a);
}

// svc_verify in verify_sig's shape: Perm is a bad signature, not a fault.
Task<Result<bool>> verify_here(Str key, Str sig, Str bytes)
{
    Task<Result<void>> t = svc_verify(key, sig, bytes);
    if (!t)
        co_return Err(Error::NoMemory);
    Result<void> r = co_await t;
    if (r.is_err())
        co_return r.error() == Error::Perm ? Result<bool>(false) : Result<bool>(Err(r.error()));
    co_return true;
}

// A browser with no Ed25519.
Task<Result<bool>> verify_absent(Str, Str, Str)
{
    co_return Err(Error::Unsupported);
}

// A namespace-scope global must be trivially destructible.
TrustVerify chosen = verify_here;
bool answered;
bool outcome;
Error failure;
usize reached;

Task<i32> ask_self(const AnchorFile *a, u64 now)
{
    Task<Result<bool>> t = trust_self(*a, now, chosen);
    if (!t)
        co_return 1;
    Result<bool> r = co_await t;
    answered       = true;
    if (r.is_err()) {
        failure = r.error();
        co_return 1;
    }
    outcome = r.value();
    co_return 0;
}

Task<i32> ask_walk(const AnchorFile *chain, usize n, u64 now)
{
    Task<Result<usize>> t = trust_walk(Span<const AnchorFile>(chain, n), now, chosen);
    if (!t)
        co_return 1;
    Result<usize> r = co_await t;
    answered        = true;
    if (r.is_err()) {
        failure = r.error();
        co_return 1;
    }
    reached = r.value();
    co_return 0;
}

void arm()
{
    sched_reset();
    answered = false;
    outcome  = false;
    reached  = 0;
    failure  = Error::Invalid;
}

bool run_self(const AnchorFile &a, u64 now = NOW)
{
    arm();
    if (sched_spawn(ask_self(&a, now)) == 0)
        return false;
    sched_tick(0);
    return answered && outcome;
}

usize run_walk(const AnchorFile *chain, usize n)
{
    arm();
    if (sched_spawn(ask_walk(chain, n, NOW)) == 0)
        return ~usize(0);
    sched_tick(0);
    return answered ? reached : ~usize(0);
}

} // namespace

void test_trust()
{
    test_begin("trust");

    AnchorFile good;
    CHECK(load("good", good));

    // §1.1: a key's name is the digest of its public key, recomputed. The
    // fixture's Y: lines name the first two root keys.
    {
        char name[DIGEST_TEXT];
        CHECK(trust_key_name(good.rec.keys[0], Span<char>(name)));
        CHECK(good.sigs[0].key_name == Str(name, sizeof(name)));
        CHECK(trust_key_name(good.rec.keys[1], Span<char>(name)));
        CHECK(good.sigs[1].key_name == Str(name, sizeof(name)));

        char narrow[DIGEST_TEXT - 1];
        CHECK(!trust_key_name(good.rec.keys[0], Span<char>(narrow)));
    }

    // §4: the record, and the thresholds a use is looked up by.
    CHECK_EQ(u32(good.rec.version), 1);
    CHECK_EQ(trust_threshold(good.rec, TRUST_ROOT), 2);
    CHECK_EQ(trust_threshold(good.rec, TRUST_INDEX), 1);
    CHECK_EQ(trust_threshold(good.rec, "nobody"), 0);

    // A good anchor loads and meets its own root threshold.
    CHECK(run_self(good));

    // §7 step 4: one signature short.
    {
        AnchorFile a;
        CHECK(load("short", a));
        CHECK(!run_self(a));
    }

    // The one this task exists for: the threshold met with one key's signature
    // repeated. Both lines verify; only one may count.
    {
        AnchorFile a;
        CHECK(load("repeat", a));
        CHECK_EQ(a.sigs.size(), 2);
        CHECK(a.sigs[0].key_name == a.sigs[1].key_name);
        CHECK(!run_self(a));
    }

    // §2: a Y: naming a key the anchor does not carry counts for nothing and
    // is not an error — one short with it, met without it.
    {
        AnchorFile a, b;
        CHECK(load("stranger", a));
        CHECK(!run_self(a));
        CHECK(load("extra", b));
        CHECK(run_self(b));
    }

    // A signature that is not 64 bytes is skipped, not fatal: the file still
    // reads, and what is left is one signature short.
    {
        AnchorFile a;
        CHECK(load("stubsig", a));
        CHECK(!run_self(a));
    }

    // The expiry, against the caller's fixed time.
    {
        AnchorFile a;
        CHECK(load("expired", a));
        CHECK(!run_self(a));
        CHECK(!run_self(good, good.rec.expiry)); // the instant it expires
        CHECK(run_self(good, good.rec.expiry - 1));
    }

    // §4's rest, refused before a verifier is reached.
    {
        AnchorFile a;
        CHECK(!load("grammar", a));  // X:2
        CHECK(!load("dupkey", a));   // one K twice
        CHECK(!load("duph", a));     // two H:root
        CHECK(!load("shortkey", a)); // an ed25519 key that is not 32 bytes
        CHECK(load("spare", a));     // another algorithm's key is left alone
        CHECK(run_self(a));
    }

    // §10: 1 to 2 to 3, each checked against the one before it and itself.
    {
        AnchorFile chain[3];
        CHECK(load("good", chain[0]));
        CHECK(load("two", chain[1]));
        CHECK(load("three", chain[2]));
        CHECK_EQ(run_walk(chain, 3), 2);

        // Withholding 2 stops the walk: 3 carries no signature by 1's keys.
        AnchorFile gap[2];
        CHECK(load("good", gap[0]));
        CHECK(load("three", gap[1]));
        CHECK_EQ(run_walk(gap, 2), 0);

        // A G that does not advance is a rollback.
        AnchorFile back[2];
        CHECK(load("two", back[0]));
        CHECK(load("good", back[1]));
        CHECK_EQ(run_walk(back, 2), 0);
    }

    // A missing algorithm is a fault, and must not read as a bad signature.
    chosen = verify_absent;
    CHECK(!run_self(good));
    CHECK(failure == Error::Unsupported);
    chosen = verify_here;

    CHECK_EQ(host_orphans(), 0);
}

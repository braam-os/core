#include "cmd/pkg/encode.h"
#include "harness.h"
#include "kernel/alloc.h"
#include "kernel/hostcall.h"
#include "kernel/jsref.h"
#include "kernel/sched.h"
#include "kernel/traits.h"
#include "svc/net.h"
#include "svc/svc.h"

// The services behind these cases are test/fakesvc.mjs. The record machinery
// is the storage one — test_hostfs covers the orphan path — so what is new
// here is the slot a reply deposits into, and its two ends: it is handed over
// on success and released when the request never happens.

namespace {

// A namespace-scope global must be trivially destructible, so what was heard
// is a flag rather than the String it came in.
WallClock clock_read;
Error failure;
bool heard_ping;
bool answered;
bool recv_closed;
usize live_dropped;

Task<i32> ask_clock()
{
    Task<Result<WallClock>> t = svc_clock();
    if (!t)
        co_return 1;
    Result<WallClock> r = co_await t;
    answered            = true;
    if (r.is_err()) {
        failure = r.error();
        co_return 1;
    }
    clock_read = r.value();
    co_return 0;
}

// What the host says about itself, which boot asks for once and keeps. The
// blank line in the middle is the contract: above it is the banner's half.
bool host_split;

Task<i32> ask_host()
{
    Task<Result<String>> t = host_info();
    if (!t)
        co_return 1;
    Result<String> r = co_await t;
    answered         = true;
    if (r.is_err()) {
        failure = r.error();
        co_return 1;
    }
    host_split = r.value().str().contains("\n\n");
    co_return 0;
}

// Open, send, receive: the loopback socket in the fake delivers to itself when
// it is the only one, so one task exercises the whole path.
Task<i32> ask_socket()
{
    Result<WebSocket> open = Err(Error::NoMemory);
    if (Task<Result<WebSocket>> t = ws_open("ws://loop"))
        open = co_await t;
    if (open.is_err()) {
        failure = open.error();
        co_return 1;
    }

    if (Task<Result<void>> t = ws_send(open.value(), "ping")) {
        Result<void> sent = co_await t;
        if (sent.is_err()) {
            failure = sent.error();
            co_return 1;
        }
    }

    if (Task<Result<String>> t = ws_recv(open.value())) {
        Result<String> got = co_await t;
        if (got.is_err()) {
            failure = got.error();
            co_return 1;
        }
        heard_ping = got.value().str() == "ping";
    }
    answered = true;
    co_return 0;
}

// What a Close under a parked read rests on: the host lets go at once, and the
// slot stays reserved until the handle does. A slot freed with the object would
// be handed straight back out, and a request already issued names it.
Task<i32> ask_dropped_socket()
{
    Result<WebSocket> open = Err(Error::NoMemory);
    if (Task<Result<WebSocket>> t = ws_open("ws://loop"))
        open = co_await t;
    if (open.is_err()) {
        failure = open.error();
        co_return 1;
    }

    open.value().sock.drop();
    live_dropped = jsref_live();

    if (Task<Result<String>> t = ws_recv(open.value())) {
        Result<String> got = co_await t;
        recv_closed        = got.is_err() && got.error() == Error::Closed;
    }
    answered = true;
    co_return 0;
}

// Ed25519, against RFC 8032 §7.1. The vectors stay hex so they can be read
// against the RFC; test/fakesvc.mjs is what actually checks them.
constexpr Str T1_KEY = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
constexpr Str T1_SIG = "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555"
                       "fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";
constexpr Str T2_KEY = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
constexpr Str T2_SIG = "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da0"
                       "85ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00";

u8 t1_key[32], t1_sig[64];
u8 t2_key[32], t2_sig[64];

// pkg's decoder rather than a second one here.
usize unhex(Str hex, Span<u8> out)
{
    Option<usize> n = hex_decode(hex, out);
    return n ? n.value() : 0;
}

Str raw(const u8 *p, usize n) { return Str(reinterpret_cast<const char *>(p), n); }

bool verify_good;

Task<i32> ask_verify(Str key, Str sig, Str msg)
{
    Task<Result<void>> t = svc_verify(key, sig, msg);
    if (!t)
        co_return 1;
    Result<void> r = co_await t;
    answered       = true;
    verify_good    = !r.is_err();
    if (r.is_err())
        failure = r.error();
    co_return 0;
}

// Raw deflate, from the zlib tools/pack.py deflates with. The short one fits
// one SYS_CHUNK; the long one takes three, which is what walks the read loop.
const u8 DEF_SHORT[] = { 0x4b, 0x2a, 0x4a, 0x4c, 0xcc, 0x4d, 0xa2, 0x80, 0x00, 0x00 };
const u8 DEF_LONG[]  = { 0x4b, 0x2a, 0x4a, 0x4c, 0xcc, 0x4d, 0x1a, 0x25, 0x46,
                         0x89, 0x51, 0x62, 0x94, 0x18, 0x25, 0x86, 0x13, 0x01, 0x00 };

usize inflated_size;
bool inflated_ok;

// Reads the stream to its end, checking it is "braam" over and over. Only a
// clean end of stream sets inflated_ok, so a refusal at either point — the
// operation or a later read — leaves it false.
Task<i32> ask_inflate(Str deflated)
{
    inflated_ok   = false;
    inflated_size = 0;

    Result<HttpResponse> open = Err(Error::NoMemory);
    if (Task<Result<HttpResponse>> t = svc_inflate(deflated))
        open = co_await t;
    answered = true;
    if (open.is_err()) {
        failure = open.error();
        co_return 1;
    }

    bool ok = true;
    for (;;) {
        Result<String> chunk = Err(Error::NoMemory);
        if (Task<Result<String>> t = stream_read(open.value()))
            chunk = co_await t;
        if (chunk.is_err()) {
            failure = chunk.error();
            co_return 1;
        }
        if (chunk.value().empty())
            break;
        Str s = chunk.value().str();
        for (usize i = 0; i < s.size(); i++)
            if (s[i] != "braam"[(inflated_size + i) % 5])
                ok = false;
        inflated_size += s.size();
    }
    inflated_ok = ok;
    co_return 0;
}

} // namespace

void test_svc()
{
    test_begin("svc");

    usize in_use = heap_stats().bytes_in_use;
    usize live   = jsref_live();

    // The wall clock, which is what the kernel's own monotonic clock cannot be.
    sched_reset();
    answered = false;
    CHECK(sched_spawn(ask_clock()) != 0);
    CHECK_EQ(sched_tick(0), -1);
    CHECK(answered);
    CHECK(clock_read.epoch_ms > 0);
    CHECK_EQ(host_orphans(), 0);

    // The host's description of itself, over the sized-twice reply that every
    // string-returning service uses.
    sched_reset();
    answered   = false;
    host_split = false;
    CHECK(sched_spawn(ask_host()) != 0);
    CHECK_EQ(sched_tick(0), -1);
    CHECK(answered);
    CHECK(host_split);
    CHECK_EQ(host_orphans(), 0);

    // A socket is a slot the host deposits into and the kernel then owns; the
    // count comes back to where it started once the handle goes away.
    sched_reset();
    answered   = false;
    heard_ping = false;
    CHECK(sched_spawn(ask_socket()) != 0);
    CHECK_EQ(sched_tick(0), -1);
    CHECK(answered);
    CHECK(heard_ping);
    CHECK_EQ(jsref_live(), live);
    CHECK_EQ(host_orphans(), 0);

    // Dropped but not released: the socket answers a read with Err(Closed),
    // which is what a reader parked on a closed descriptor unwinds on, and the
    // slot is still counted until the handle goes.
    sched_reset();
    answered     = false;
    recv_closed  = false;
    live_dropped = 0;
    CHECK(sched_spawn(ask_dropped_socket()) != 0);
    CHECK_EQ(sched_tick(0), -1);
    CHECK(answered);
    CHECK(recv_closed);
    CHECK_EQ(live_dropped, live + 1);
    CHECK_EQ(jsref_live(), live);
    CHECK_EQ(host_orphans(), 0);

    // A request cancelled before it is issued never reaches the host, so the
    // slot it claimed for the reply has to come back with the record. It is
    // the one way the externref table can leak without anyone noticing.
    sched_reset();
    answered = false;
    failure  = Error::Invalid;
    u32 pid  = sched_spawn(ask_socket());
    CHECK(pid != 0);
    sched_cancel(pid);
    CHECK_EQ(sched_tick(0), -1);
    CHECK(!answered);
    CHECK(failure == Error::Cancelled);
    CHECK_EQ(jsref_live(), live);
    CHECK_EQ(host_orphans(), 0);

    // Ed25519. A good signature is Ok, a bad one Err(Perm) — an answer, not a
    // fault — and a browser with no algorithm is Err(Unsupported), which must
    // never look like either.
    CHECK_EQ(unhex(T1_KEY, Span<u8>(t1_key)), sizeof(t1_key));
    CHECK_EQ(unhex(T1_SIG, Span<u8>(t1_sig)), sizeof(t1_sig));
    CHECK_EQ(unhex(T2_KEY, Span<u8>(t2_key)), sizeof(t2_key));
    CHECK_EQ(unhex(T2_SIG, Span<u8>(t2_sig)), sizeof(t2_sig));

    Str t1_key_s = raw(t1_key, sizeof(t1_key));
    Str t1_sig_s = raw(t1_sig, sizeof(t1_sig));
    Str t2_key_s = raw(t2_key, sizeof(t2_key));
    Str t2_sig_s = raw(t2_sig, sizeof(t2_sig));

    // TEST 1's message is empty and TEST 2's is one byte, 0x72.
    Str t2_msg = "\x72";

    struct {
        Str key, sig, msg;
        bool want;
        Str what;
    } vectors[] = {
        { t1_key_s, t1_sig_s, Str(), true, "RFC 8032 TEST 1" },
        { t2_key_s, t2_sig_s, t2_msg, true, "RFC 8032 TEST 2" },
        { t2_key_s, t2_sig_s, "\x73", false, "a tampered message" },
        { t1_key_s, t2_sig_s, t2_msg, false, "a signature by the wrong key" },
        { t2_key_s, t1_sig_s, t2_msg, false, "the wrong signature" },
    };

    for (auto &v : vectors) {
        test_begin(v.what); // so a failure names the vector, not just the line
        sched_reset();
        answered    = false;
        verify_good = false;
        failure     = Error::Invalid;
        CHECK(sched_spawn(ask_verify(v.key, v.sig, v.msg)) != 0);
        CHECK_EQ(sched_tick(0), -1);
        CHECK(answered);
        CHECK_EQ(verify_good, v.want);
        if (!v.want)
            CHECK(failure == Error::Perm);
        CHECK_EQ(host_orphans(), 0);
    }

    // A key of the wrong length is Invalid, not Perm: it is malformed rather
    // than a signature that failed. The syscall arm refuses it before the host
    // is asked; here the call is direct, so the host is what refuses.
    test_begin("svc: a short key");
    sched_reset();
    answered    = false;
    verify_good = false;
    failure     = Error::Perm;
    CHECK(sched_spawn(ask_verify(t2_key_s.substr(0, 31), t2_sig_s, t2_msg)) != 0);
    CHECK_EQ(sched_tick(0), -1);
    CHECK(answered);
    CHECK(!verify_good);
    CHECK(failure == Error::Invalid);
    CHECK_EQ(host_orphans(), 0);

    // Inflate: payload in, descriptor out, read like a fetched body.
    test_begin("svc: inflate, one chunk");
    sched_reset();
    answered = false;
    CHECK(sched_spawn(ask_inflate(raw(DEF_SHORT, sizeof(DEF_SHORT)))) != 0);
    CHECK_EQ(sched_tick(0), -1);
    CHECK(answered);
    CHECK(inflated_ok);
    CHECK_EQ(inflated_size, 65);
    CHECK_EQ(jsref_live(), live);
    CHECK_EQ(host_orphans(), 0);

    // Past one SYS_CHUNK, so the whole 1500 bytes only arrive if the loop runs.
    test_begin("svc: inflate, three chunks");
    sched_reset();
    answered = false;
    CHECK(sched_spawn(ask_inflate(raw(DEF_LONG, sizeof(DEF_LONG)))) != 0);
    CHECK_EQ(sched_tick(0), -1);
    CHECK(answered);
    CHECK(inflated_ok);
    CHECK_EQ(inflated_size, 1500);
    CHECK_EQ(jsref_live(), live);
    CHECK_EQ(host_orphans(), 0);

    // Truncated: an error somewhere, never a short read that looks like an end.
    // Where it lands differs — the fake refuses the operation, a browser fails
    // a later read — so the case asserts the outcome and not the timing.
    test_begin("svc: inflate, truncated");
    sched_reset();
    answered = false;
    CHECK(sched_spawn(ask_inflate(raw(DEF_LONG, sizeof(DEF_LONG) - 2))) != 0);
    CHECK_EQ(sched_tick(0), -1);
    CHECK(answered);
    CHECK(!inflated_ok);
    CHECK(inflated_size < 1500);
    CHECK_EQ(jsref_live(), live);
    CHECK_EQ(host_orphans(), 0);

    test_begin("svc");

    sched_reset();
    CHECK_EQ(heap_stats().bytes_in_use, in_use);
}

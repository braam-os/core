#include "fs/hostfs.h"
#include "harness.h"
#include "kernel/alloc.h"
#include "kernel/sched.h"

// The storage backend behind these cases is test/fakefs.mjs, which answers
// from inside the import — so a request is issued and replied to within one
// tick. That is not what a browser does, and the difference does not reach
// here: wake() only queues a resumption either way.

namespace {

StorageBackend backend;
Error failure;
bool answered;

Task<i32> ask()
{
    Task<Result<StorageBackend>> t = storage_info();
    if (!t)
        co_return 1;

    Result<StorageBackend> r = co_await t;
    answered                 = true;
    if (r.is_err()) {
        failure = r.error();
        co_return 1;
    }
    backend = r.value();
    co_return 0;
}

} // namespace

void test_hostfs()
{
    test_begin("hostfs");

    usize in_use = heap_stats().bytes_in_use;

    // A round trip through host_fs: the request record goes out, the reply
    // comes back through wake(), and the awaiting task resumes with it.
    sched_reset();
    answered = false;
    CHECK(sched_spawn(ask()) != 0);
    CHECK_EQ(sched_tick(0), -1);
    CHECK(answered);
    CHECK(backend.opfs);
    CHECK(backend.sync);
    CHECK(backend.quota > 0);
    CHECK_EQ(fs_orphans(), 0);

    // A task cancelled before it reaches the import never issues a request, so
    // the record is this side's to free rather than something to orphan. The
    // heap check at the end is what proves it.
    sched_reset();
    answered = false;
    failure  = Error::Invalid;
    u32 pid  = sched_spawn(ask());
    CHECK(pid != 0);
    sched_cancel(pid);
    CHECK_EQ(sched_tick(0), -1);
    CHECK(answered);
    CHECK(failure == Error::Cancelled);
    CHECK_EQ(fs_orphans(), 0);

    // A reply for a token nothing is waiting on is the ordinary case for a
    // late event, and must not disturb anything.
    fs_orphan_reply(12345);
    CHECK_EQ(fs_orphans(), 0);

    sched_reset();
    CHECK_EQ(heap_stats().bytes_in_use, in_use);
}

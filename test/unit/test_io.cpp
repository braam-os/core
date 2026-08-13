#include "harness.h"
#include "kernel/alloc.h"
#include "kernel/sched.h"
#include "kernel/str.h"
#include "user/io.h"

namespace {

char trace[64];
usize trace_n;

void mark(char c)
{
    if (trace_n < sizeof(trace))
        trace[trace_n++] = c;
}

Str traced()
{
    return Str(trace, trace_n);
}

// One write per character, so parking is visible a slot at a time.
Task<i32> writer(Stream out, Str s)
{
    for (usize i = 0; i < s.size(); i++) {
        Result<usize> r = co_await out.write(s.substr(i, 1));
        if (r.is_err()) {
            mark(r.error() == Error::Closed ? 'p' : 'x');
            co_return 1;
        }
        mark(s[i]);
    }
    co_return 0;
}

// Reads to end of input, bracketing each line.
Task<i32> liner(Source in)
{
    LineReader lr(in);
    String line;
    for (;;) {
        Result<bool> r = co_await lr.next(line);
        if (r.is_err()) {
            mark('x');
            co_return 1;
        }
        if (!r.value()) {
            mark('.');
            co_return 0;
        }
        mark('[');
        for (usize i = 0; i < line.size(); i++)
            mark(line[i]);
        mark(']');
    }
}

bool send_chunk(Pipe &p, Str s)
{
    String chunk;
    return chunk.assign(s) && p.try_send(move(chunk));
}

} // namespace

void test_io()
{
    test_begin("io");

    // A full pipe parks the writer; each read lets exactly one more through.
    sched_reset();
    trace_n = 0;
    {
        Pipe p;
        u32 pid = sched_spawn(writer(pipe_sink(p), "0123456789"));
        CHECK_EQ(sched_tick(0), -1);
        CHECK(traced() == "01234567"); // PIPE_SLOTS of them, then parked
        CHECK(p.full());
        CHECK(sched_alive(pid));

        CHECK(p.try_recv().has_value());
        CHECK_EQ(sched_tick(1), -1);
        CHECK(traced() == "012345678");

        // The reader leaving is what stops the writer, which is how `head`
        // ends an upstream producer.
        p.hangup();
        CHECK_EQ(sched_tick(2), -1);
        CHECK(traced() == "012345678p");
        CHECK(!sched_alive(pid));
        sched_reset();
    }

    // A write to a sink whose reader has already gone reports Closed at once.
    trace_n = 0;
    {
        Pipe p;
        p.hangup();
        sched_spawn(writer(pipe_sink(p), "ab"));
        CHECK_EQ(sched_tick(0), -1);
        CHECK(traced() == "p");
        sched_reset();
    }

    // Lines span chunks, and a final fragment with no newline is a line.
    trace_n = 0;
    {
        Pipe p;
        sched_spawn(liner(pipe_source(p)));
        CHECK_EQ(sched_tick(0), -1);
        CHECK(traced() == "");

        CHECK(send_chunk(p, "hello\nwor"));
        CHECK_EQ(sched_tick(1), -1);
        CHECK(traced() == "[hello]");

        CHECK(send_chunk(p, "ld\nlast"));
        p.close();
        CHECK_EQ(sched_tick(2), -1);
        CHECK(traced() == "[hello][world][last].");
        sched_reset();
    }

    // Empty lines survive, and so does a chunk that is only a newline.
    trace_n = 0;
    {
        Pipe p;
        sched_spawn(liner(pipe_source(p)));
        CHECK(send_chunk(p, "a\n\n"));
        CHECK(send_chunk(p, "\n"));
        p.close();
        CHECK_EQ(sched_tick(0), -1);
        CHECK(traced() == "[a][][].");
        sched_reset();
    }

    // A source with nothing behind it is end of input, not an error.
    trace_n = 0;
    sched_spawn(liner(null_source()));
    CHECK_EQ(sched_tick(0), -1);
    CHECK(traced() == ".");
    sched_reset();

    // Cancelling a reader parked on an empty pipe unwinds it, and cancelling
    // a writer parked on a full one does the same.
    trace_n      = 0;
    usize in_use = heap_stats().bytes_in_use;
    {
        Pipe p;
        u32 pid = sched_spawn(liner(pipe_source(p)));
        CHECK_EQ(sched_tick(0), -1);
        sched_cancel(pid);
        CHECK_EQ(sched_tick(1), -1);
        CHECK(traced() == "x");
        CHECK(!sched_alive(pid));
        sched_reset();
    }
    trace_n = 0;
    {
        Pipe p;
        u32 pid = sched_spawn(writer(pipe_sink(p), "0123456789"));
        CHECK_EQ(sched_tick(0), -1);
        sched_cancel(pid);
        CHECK_EQ(sched_tick(1), -1);
        CHECK(traced() == "01234567x");
        CHECK(!sched_alive(pid));
        CHECK_EQ(p.size(), PIPE_SLOTS); // the cancelled write delivered nothing
        sched_reset();
    }

    // Destroying a frame parked in a write deregisters its waiter, so the
    // pipe is still usable afterwards.
    trace_n = 0;
    {
        Pipe p;
        sched_spawn(writer(pipe_sink(p), "0123456789"));
        CHECK_EQ(sched_tick(0), -1);
        sched_reset();
        CHECK_EQ(p.size(), PIPE_SLOTS);
        for (usize i = 0; i < PIPE_SLOTS; i++)
            CHECK(p.try_recv().has_value());
        CHECK(p.empty());
    }
    sched_reset();
    CHECK_EQ(heap_stats().bytes_in_use, in_use);
}

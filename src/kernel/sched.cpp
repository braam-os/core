#include "sched.h"

#include "alloc.h"
#include "hash.h"
#include "host.h"
#include "sysabi.h" // SYS_PID_MAX, which bounds the pid table's ids
#include "traits.h"
#include "vec.h"

namespace {

struct Timer {
    f64 deadline = 0;
    Waiter *w    = nullptr;
};

struct Job {
    u32 pid     = 0;
    f64 started = 0; // when it was spawned, for /proc's elapsed time
    Str name;        // a view: the caller's literal, or a Program's own name
    Task<i32> root;
    CancelState cancel;
};

// The tables hold freed pointers while ~Sched walks them, so anything a frame
// destructor might reach that iterates one has to stand down.
bool tearing_down = false;

// A pid that outlives its job, and how many things still name it.
struct Hold {
    u32 pid   = 0;
    u32 count = 0;
};

// Destroys a table's jobs newest first: a child is spawned after its parent and
// its frame may hold a reference to something the parent's frame owns.
void drop_all(Vec<Job *> &t)
{
    for (usize i = t.size(); i > 0; i--) {
        t[i - 1]->~Job();
        heap_free(t[i - 1]);
    }
}

struct Sched {
    Vec<std::coroutine_handle<>> ready;
    usize head = 0; // Vec has no pop_front; the queue is drained with a cursor
    Vec<Timer> timers;
    HashMap<u32, Waiter *> waits;

    // Two tables, two id spaces. `jobs` is 1..SYS_PID_MAX and is what /proc
    // lists; `anon` is above it, for the jobs the kernel runs for itself.
    Vec<Job *> jobs;
    Vec<Job *> anon;
    Vec<Hold> held; // pids reserved past their job (sched_pid_hold)
    u32 next_pid   = 1;
    u32 next_anon  = SYS_PID_MAX + 1;
    u32 next_token = 1;
    f64 now        = 0;

    // SchedStats' counters. Members rather than globals, so a reset zeroes them.
    u64 ticks        = 0;
    u64 resumes      = 0;
    u64 wakes        = 0;
    u64 unparks      = 0;
    u64 misses       = 0;
    u64 timers_fired = 0; // `timers` above is the queue
    u64 spawns       = 0;
    u64 wraps        = 0;

    // Destroying a job destroys its suspended frames, whose awaiters deregister
    // from the queues above; those must still be alive here. `anon` first: a
    // syscall server is spawned after the process it serves and its frame holds
    // a ProcRef and awaitables parked on that process's stdio, so it must unwind
    // before the stepper does.
    ~Sched()
    {
        tearing_down = true;
        drop_all(anon);
        drop_all(jobs);
    }
};

// Constructed on first use: a global with a non-trivial destructor would need
// static init, which --no-entry never runs.
Sched *g = nullptr;

Sched &sched()
{
    if (!g) {
        g = static_cast<Sched *>(heap_alloc(sizeof(Sched)));
        if (!g)
            panic("sched: out of memory");
        new (g) Sched();
    }
    return *g;
}

void push_ready(std::coroutine_handle<> h)
{
    if (!sched().ready.push(h))
        panic("sched: ready queue out of memory");
}

std::coroutine_handle<> pop_ready()
{
    Sched &s = sched();
    if (s.head >= s.ready.size()) {
        s.ready.clear();
        s.head = 0;
        return nullptr;
    }
    std::coroutine_handle<> h = s.ready[s.head++];
    if (s.head == s.ready.size()) {
        s.ready.clear();
        s.head = 0;
    }
    return h;
}

// Sorted by deadline, earliest last, so firing pops from the back.
bool insert_timer(Waiter *w, f64 deadline)
{
    Sched &s = sched();
    if (!s.timers.push(Timer{ deadline, w }))
        return false;
    for (usize i = s.timers.size() - 1; i > 0 && s.timers[i - 1].deadline < deadline; i--)
        swap(s.timers[i - 1], s.timers[i]);
    return true;
}

void remove_timer(Waiter *w)
{
    Sched &s = sched();
    for (usize i = 0; i < s.timers.size(); i++) {
        if (s.timers[i].w != w)
            continue;
        for (usize j = i + 1; j < s.timers.size(); j++)
            s.timers[j - 1] = s.timers[j];
        s.timers.pop();
        return;
    }
}

// Where a suspended task is registered. A channel and a host call both wait on
// a token, so `parked` is the awaiter saying which. Shared by /proc's per-task
// line and by the gauges, so the two cannot disagree.
ProcInfo::Wait wait_kind(const Waiter *w)
{
    return !w          ? ProcInfo::Wait::None
           : w->timed  ? ProcInfo::Wait::Timer
           : w->parked ? ProcInfo::Wait::Park
           : w->listed ? ProcInfo::Wait::Host
                       : ProcInfo::Wait::None;
}

// The table an id belongs to. The two spaces are disjoint, so this is the whole
// of telling a kernel job from a task userland can name.
Vec<Job *> &table_of(u32 pid)
{
    Sched &s = sched();
    return pid > SYS_PID_MAX ? s.anon : s.jobs;
}

// One table's share of the gauges. `ready` is a task suspended on nothing, not
// the depth of the ready queue: this runs inside the drain, where that depth is
// the reader's own backlog.
void count_waits(const Vec<Job *> &t, SchedStats &out)
{
    for (Job *j : t) {
        const Waiter *w = j->cancel.waiting;
        if (!w) {
            out.ready++; // what `ps` prints as R
            continue;
        }
        switch (wait_kind(w)) {
        case ProcInfo::Wait::Timer:
            out.on_timer++;
            break;
        case ProcInfo::Wait::Park:
            out.on_park++;
            break;
        case ProcInfo::Wait::Host:
            out.on_host++;
            break;
        case ProcInfo::Wait::None:
            break; // suspended but registered nowhere: a failed await, unwinding
        }
    }
}

// Frees the jobs whose root has returned, keeping the rest in order.
void reap(Vec<Job *> &t)
{
    usize k = 0;
    for (usize i = 0; i < t.size(); i++) {
        Job *j = t[i];
        if (j->root.done()) {
            j->~Job();
            heap_free(j);
        } else {
            t[k++] = j;
        }
    }
    t.resize(k);
}

Job *find_job(u32 pid)
{
    if (tearing_down)
        return nullptr;
    for (Job *j : table_of(pid))
        if (j->pid == pid)
            return j;
    return nullptr;
}

// Is this id spoken for — a live job, or a pid something still names?
bool id_taken(u32 pid)
{
    for (Job *j : table_of(pid))
        if (j->pid == pid)
            return true;
    for (const Hold &h : sched().held)
        if (h.pid == pid)
            return true;
    return false;
}

// The next free id in `pid`'s space, wrapping. At most one candidate more than
// there are jobs and holds, since that many consecutive ids cannot all be
// taken; 0 is "the space is full", which is a spawn failure.
u32 take_id(JobId kind)
{
    Sched &s   = sched();
    bool anon  = kind == JobId::Anon;
    u32 &next  = anon ? s.next_anon : s.next_pid;
    u32 lowest = anon ? SYS_PID_MAX + 1 : 1;
    u32 top    = anon ? 0xffffffff : SYS_PID_MAX;

    for (usize tries = s.jobs.size() + s.anon.size() + s.held.size() + 1; tries > 0; tries--) {
        u32 id = next;
        if (next == top) {
            next = lowest;
            if (!anon)
                s.wraps++;
        } else {
            next++;
        }
        if (!id_taken(id))
            return id;
    }
    return 0;
}

} // namespace

u32 sched_spawn(Task<i32> t, Str name, JobId id)
{
    if (!t)
        return 0;

    Sched &s = sched();

    u32 pid = take_id(id);
    if (!pid)
        return 0;

    Job *j = static_cast<Job *>(heap_alloc(sizeof(Job)));
    if (!j)
        return 0;
    new (j) Job();

    j->pid                            = pid;
    j->started                        = s.now;
    j->name                           = name;
    j->root                           = move(t);
    j->root.handle().promise().cancel = &j->cancel;
    if (!table_of(pid).push(j)) {
        j->~Job();
        heap_free(j);
        return 0;
    }
    s.spawns++;
    push_ready(j->root.handle());
    return j->pid;
}

bool sched_pid_hold(u32 pid)
{
    if (!pid || pid > SYS_PID_MAX)
        return true;
    Sched &s = sched();
    for (Hold &h : s.held)
        if (h.pid == pid) {
            h.count++;
            return true;
        }
    return s.held.push(Hold{ pid, 1 });
}

void sched_pid_drop(u32 pid)
{
    if (!pid || pid > SYS_PID_MAX)
        return;
    Sched &s = sched();
    for (usize i = 0; i < s.held.size(); i++)
        if (s.held[i].pid == pid) {
            if (--s.held[i].count == 0)
                s.held.erase(i);
            return;
        }
}

void sched_cancel(u32 pid)
{
    Job *j = find_job(pid);
    if (!j || j->cancel.cancelled)
        return;

    j->cancel.cancelled = true;

    // A suspended tree has to be put back on the ready queue to observe it;
    // one already running will see the flag at its next await point.
    if (Waiter *w = j->cancel.waiting) {
        sched_unwait(w);
        w->cancelled = true;
        push_ready(w->h);
    }
}

bool sched_alive(u32 pid)
{
    return find_job(pid) != nullptr;
}

// The pid table alone: an anonymous job is the kernel's own business and has no
// line in /proc.
usize sched_procs(ProcInfo *out, usize cap)
{
    if (tearing_down)
        return 0;

    Sched &s = sched();
    usize n  = 0;
    for (Job *j : s.jobs) {
        if (n == cap)
            break;
        out[n].pid       = j->pid;
        out[n].name      = j->name;
        out[n].waiting   = j->cancel.waiting != nullptr;
        out[n].cancelled = j->cancel.cancelled;
        out[n].started   = j->started;

        out[n].wait = wait_kind(j->cancel.waiting);
        n++;
    }
    return n;
}

SchedStats sched_stats()
{
    Sched &s = sched();

    SchedStats out{};
    out.ticks   = s.ticks;
    out.resumes = s.resumes;
    out.wakes   = s.wakes;
    out.unparks = s.unparks;
    out.misses  = s.misses;
    out.timers  = s.timers_fired;
    out.spawns  = s.spawns;

    out.wraps = s.wraps;

    // The gauges are read off the tables, which hold freed pointers during
    // teardown.
    if (tearing_down)
        return out;

    // Both tables, so the count includes what /proc does not list.
    out.tasks = s.jobs.size() + s.anon.size();
    count_waits(s.jobs, out);
    count_waits(s.anon, out);
    return out;
}

i32 sched_tick(f64 now_ms)
{
    Sched &s = sched();
    s.now    = now_ms;
    s.ticks++;

    while (!s.timers.empty() && s.timers.back().deadline <= now_ms) {
        Waiter *w = s.timers.back().w;
        s.timers.pop();
        w->timed = false;
        if (w->cancel && w->cancel->waiting == w)
            w->cancel->waiting = nullptr;
        s.timers_fired++;
        push_ready(w->h);
    }

    // Resuming may queue more work; the drain runs until the queue is empty.
    while (std::coroutine_handle<> h = pop_ready()) {
        s.resumes++;
        h.resume();
    }

    reap(s.jobs);
    reap(s.anon);

    if (s.head < s.ready.size())
        return 0;
    if (s.timers.empty())
        return -1;

    f64 d = s.timers.back().deadline - now_ms;
    if (d < 0)
        d = 0;
    if (d > 0x3fffffff)
        d = 0x3fffffff;
    return i32(d + 0.999); // round up, so the host never wakes early
}

bool sched_wake(u32 token, u32 ptr, u32 len)
{
    Sched &s      = sched();
    Waiter **slot = s.waits.find(token);
    if (!slot) {
        s.misses++;
        return false; // nothing waits on it: a late or cancelled event
    }

    Waiter *w = *slot;

    // Only one of the four callers is the host: a channel wakes its peer, and an
    // exiting child wakes a parked parent. Split the way /proc splits a wait.
    if (w->parked)
        s.unparks++;
    else
        s.wakes++;

    s.waits.remove(token);
    w->listed      = false;
    w->payload_ptr = ptr;
    w->payload_len = len;
    if (w->cancel && w->cancel->waiting == w)
        w->cancel->waiting = nullptr;
    push_ready(w->h);
    return true;
}

f64 sched_now()
{
    return sched().now;
}

usize sched_pending()
{
    return sched().jobs.size() + sched().anon.size();
}

void sched_reset()
{
    if (!g)
        return;
    Sched *s = g;
    s->~Sched(); // g stays set: deregistration during teardown needs it
    heap_free(s);
    g            = nullptr;
    tearing_down = false;
}

void sched_pid_seed(u32 pid, u32 anon)
{
    Sched &s    = sched();
    s.next_pid  = pid;
    s.next_anon = anon;
}

u32 sched_token()
{
    Sched &s = sched();
    u32 t    = s.next_token++;
    if (s.next_token == 0)
        s.next_token = 1; // 0 means "not registered"
    return t;
}

bool sched_wait_timer(Waiter *w, u32 ms)
{
    Sched &s = sched();
    if (!insert_timer(w, s.now + f64(ms)))
        return false;
    w->timed = true;
    if (w->cancel)
        w->cancel->waiting = w;
    return true;
}

bool sched_wait_token(Waiter *w)
{
    Sched &s = sched();
    if (!s.waits.insert(w->token, w))
        return false;
    w->listed = true;
    if (w->cancel)
        w->cancel->waiting = w;
    return true;
}

// Called from every awaiter's destructor, so destroying a suspended frame
// never leaves a pointer into it behind.
void sched_unwait(Waiter *w)
{
    if (!g)
        return;
    if (w->timed) {
        remove_timer(w);
        w->timed = false;
    }
    if (w->listed) {
        g->waits.remove(w->token);
        w->listed = false;
    }
    if (w->cancel && w->cancel->waiting == w)
        w->cancel->waiting = nullptr;
}

Task<Result<void>> sleep_ms(u32 ms)
{
    co_return co_await Sleep(ms);
}

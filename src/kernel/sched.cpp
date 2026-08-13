#include "sched.h"

#include "alloc.h"
#include "hash.h"
#include "host.h"
#include "traits.h"
#include "vec.h"

namespace {

struct Timer {
    f64 deadline = 0;
    Waiter *w    = nullptr;
};

struct Job {
    u32 pid = 0;
    Task<i32> root;
    CancelState cancel;
};

// jobs[] holds freed pointers while ~Sched walks it, so anything a frame
// destructor might reach that iterates jobs has to stand down.
bool tearing_down = false;

struct Sched {
    Vec<std::coroutine_handle<>> ready;
    usize head = 0; // Vec has no pop_front; the queue is drained with a cursor
    Vec<Timer> timers;
    HashMap<u32, Waiter *> waits;
    Vec<Job *> jobs;
    u32 next_pid   = 1;
    u32 next_token = 1;
    f64 now        = 0;

    // Destroying a job destroys its suspended frames, whose awaiters
    // deregister from the queues above; those must still be alive here.
    // Backwards, because a child is spawned after its parent and its frame
    // may hold a reference to something the parent's frame owns.
    ~Sched()
    {
        tearing_down = true;
        for (usize i = jobs.size(); i > 0; i--) {
            jobs[i - 1]->~Job();
            heap_free(jobs[i - 1]);
        }
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

Job *find_job(u32 pid)
{
    if (tearing_down)
        return nullptr;
    for (Job *j : sched().jobs)
        if (j->pid == pid)
            return j;
    return nullptr;
}

} // namespace

u32 sched_spawn(Task<i32> t)
{
    if (!t)
        return 0;

    Sched &s = sched();
    Job *j   = static_cast<Job *>(heap_alloc(sizeof(Job)));
    if (!j)
        return 0;
    new (j) Job();

    j->pid                            = s.next_pid++;
    j->root                           = move(t);
    j->root.handle().promise().cancel = &j->cancel;
    if (!s.jobs.push(j)) {
        j->~Job();
        heap_free(j);
        return 0;
    }
    push_ready(j->root.handle());
    return j->pid;
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

i32 sched_tick(f64 now_ms)
{
    Sched &s = sched();
    s.now    = now_ms;

    while (!s.timers.empty() && s.timers.back().deadline <= now_ms) {
        Waiter *w = s.timers.back().w;
        s.timers.pop();
        w->timed = false;
        if (w->cancel && w->cancel->waiting == w)
            w->cancel->waiting = nullptr;
        push_ready(w->h);
    }

    // Resuming may queue more work; the drain runs until the queue is empty.
    while (std::coroutine_handle<> h = pop_ready())
        h.resume();

    usize k = 0;
    for (usize i = 0; i < s.jobs.size(); i++) {
        Job *j = s.jobs[i];
        if (j->root.done()) {
            j->~Job();
            heap_free(j);
        } else {
            s.jobs[k++] = j;
        }
    }
    s.jobs.resize(k);

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

void sched_wake(u32 token, u32 ptr, u32 len)
{
    Sched &s      = sched();
    Waiter **slot = s.waits.find(token);
    if (!slot)
        return; // nothing waits on it: a late or cancelled event

    Waiter *w = *slot;
    s.waits.remove(token);
    w->listed      = false;
    w->payload_ptr = ptr;
    w->payload_len = len;
    if (w->cancel && w->cancel->waiting == w)
        w->cancel->waiting = nullptr;
    push_ready(w->h);
}

f64 sched_now()
{
    return sched().now;
}

usize sched_pending()
{
    return sched().jobs.size();
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

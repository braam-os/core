#include "proctab.h"

#include "exec.h"

namespace {

// Lazily built, like the orphan list: a namespace-scope Vec has a destructor,
// and nothing provides __cxa_atexit.
Vec<Proc *> *g_procs;

} // namespace

void pipe_release(ProcPipe *q)
{
    if (--q->refs == 0)
        heap_delete(q);
}

void handle_release(Handle *h)
{
    if (h && --h->refs == 0)
        heap_delete(h);
}

Proc *proc_find(u32 pid)
{
    if (!g_procs)
        return nullptr;
    for (Proc *p : *g_procs)
        if (p->pid == pid)
            return p;
    return nullptr;
}

bool proc_add(Proc *p)
{
    if (!g_procs)
        g_procs = heap_new<Vec<Proc *>>();
    return g_procs && g_procs->push(p);
}

void proc_remove(Proc *p)
{
    if (!g_procs)
        return;
    for (usize i = 0; i < g_procs->size(); i++)
        if ((*g_procs)[i] == p) {
            g_procs->erase(i);
            return;
        }
}

void proc_release(Proc *p)
{
    if (--p->refs == 0)
        heap_delete(p);
}

Call *proc_staging(Proc &p)
{
    if (!p.staging)
        p.staging = heap_new<Call>();
    return p.staging;
}

u32 proc_stage(Proc &p, u32 n)
{
    // A hostile binary can call Sys::Stage directly, so the size it asks for
    // is bounded by the largest payload the ABI has: a blit of the whole grid.
    if (n > SYS_STAGE_MAX)
        return 0;
    Call *c = proc_staging(p);
    if (!c)
        return 0;
    if (n > c->cap) {
        u8 *q = static_cast<u8 *>(heap_alloc(n));
        if (!q)
            return 0;
        heap_free(c->stage);
        c->stage = q;
        c->cap   = heap_block_size(n);
    }
    return host_addr(c->stage);
}

Handle *proc_handle(Proc &p, u32 fd)
{
    if (fd < SYS_FD_MIN || fd - SYS_FD_MIN >= p.fds.size())
        return nullptr;
    return p.fds[fd - SYS_FD_MIN];
}

i32 proc_bind(Proc &p, Handle *h)
{
    usize slot = p.fds.size();
    for (usize i = 0; i < p.fds.size(); i++)
        if (!p.fds[i]) {
            slot = i;
            break;
        }
    if (slot == p.fds.size() && !p.fds.push(h))
        return -1;
    p.fds[slot] = h;
    return i32(slot + SYS_FD_MIN);
}

i32 handle_bind(Proc &p, Handle *h)
{
    i32 fd = proc_bind(p, h);
    if (fd >= 0)
        return fd;
    handle_release(h); // closes it
    return -i32(Error::NoMemory);
}

bool exec_proc_state(u32 pid, ProcState &out)
{
    Proc *p = proc_find(pid);
    if (!p) {
        // Not a process, but perhaps a syscall server of one: those are ordinary
        // jobs, and naming their owner is what keeps /proc legible. Only while
        // the call is outstanding — once it is answered the record is erased and
        // what is left is a coroutine finishing, which is what /proc then says.
        if (g_procs)
            for (Proc *q : *g_procs)
                for (const Call *c : q->calls)
                    if (c->server == pid) {
                        out.ppid = q->pid;
                        return true;
                    }
        return false;
    }

    out.worker    = true;
    out.calls     = u32(p->calls.size());
    out.pages     = p->pages;
    out.max_pages = p->max_pages;
    out.cwd       = p->cwd.str();

    // A closed descriptor leaves its slot behind as a null, so the slots are
    // not the count.
    out.fds = 0;
    for (Handle *h : p->fds)
        if (h)
            out.fds++;

    // Upwards by search: a record holds its children and not its parent, and
    // this is the same scan the exit status takes to find whom to report to.
    out.ppid = 0;
    if (g_procs)
        for (Proc *q : *g_procs)
            for (const Child &ch : q->children)
                if (ch.pid == pid)
                    out.ppid = q->pid;
    return true;
}

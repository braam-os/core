#include "job.h"

#include "expand.h"
#include "glob.h"
#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/traits.h"
#include "kernel/vec.h"
#include "match.h"
#include "parse.h"
#include "shell.h"
#include "var.h"

namespace {

struct FuncEntry;

// One stage, once its descriptors are decided. A number below SYS_FD_MIN is
// "the stream this shell was given"; anything else is a descriptor of ours,
// which a spawn *moves* out of our table and a builtin only borrows.
struct Stage {
    u32 in  = SYS_STDIN;
    u32 out = SYS_STDOUT;
    u32 err = SYS_STDERR;

    const FuncEntry *fn = nullptr; // looked up ahead of a builtin and of /bin
    const Builtin *b    = nullptr;
    u32 pid             = 0;
    i32 status          = 0;
    bool moved          = false; // its three fds went into a child
};

// One variable an assignment prefix displaced.
struct Saved {
    String name, value;
    bool was_set = false;
};

// Everything a running pipeline owns, on the heap rather than in run_line's
// frame: the allocator's top size class is 512 bytes and a frame past it costs
// a whole 64 KiB span (Concept.md §8.2). One per pipeline, not per line:
// two live at once the moment a substitution runs a pipeline inside a word.
struct Run {
    const Tree *t = nullptr; // borrowed: run_line outlives every Run under it
    u32 cmd0      = 0;       // this pipeline's slice of the tree's commands
    u32 ncmds     = 0;

    Vec<Stage> stages;
    Vec<u32> spare; // descriptors nobody took, closed on the way out

    Vec<Field> words; // every stage's argv, then every redirection's target
    Vec<Str> argv;    // views over words, contiguous per command
    Vec<usize> argv0; // where each command's argv starts in argv
    Vec<Str> targets; // one per Redirect, in the order the pipeline lists them

    Vec<Field> avals;   // an assignment prefix's values, one word each
    Vec<usize> assign0; // where each command's start in avals
    Vec<Saved> saved;   // what a prefix displaced, until the command is done

    // The read end of the capture pipe, when a substitution is running this
    // pipeline. Held here rather than in a stage or in `spare`, since the
    // sweep would close it before it could be drained.
    u32 capture_r = 0;

    Args args(usize i) const
    {
        usize a = argv0[i];
        usize b = i + 1 < argv0.size() ? argv0[i + 1] : argv.size();
        return Args{ Span<const Str>(argv.data() + a, b - a) };
    }
};

// There are no exceptions, so a transfer of control is a field. A loop
// consumes Break and Continue, a function call Return; run_line the rest.
enum class Flow : u8 { Normal, Break, Continue, Return, Exit, Interrupt };

// What the walk threads through the tree. Its own block, not a frame's: it
// grows with every stage left, and run_line's frame is a recursion's root.
struct Ctx {
    static constexpr u32 MAX_DEPTH = 64;

    Str line;
    Flow flow        = Flow::Normal;
    i32 exit_status  = 0;
    u32 depth        = 0; // exec_node recursion
    u32 breaks       = 0; // levels `break` has left to unwind
    u32 loops        = 0; // how many are running
    u32 frames       = 0; // function calls and sourced files, for `return`
    bool interactive = false;

    // Where stdout goes when a `$(…)` is running this walk. Set on the whole
    // tree, so every pipeline in `$(a; b)` appends to the same string.
    String *capture = nullptr;
};

// What `break` and `continue` asked for, until the pipeline they ran in picks
// it up. 0 is "nothing asked".
u32 g_break;
bool g_break_cont;

// The same, for `return`.
bool g_return;
i32 g_return_status;

// One function, and the tree its body is a node of.
struct FuncEntry {
    String name;
    const Tree *t = nullptr; // held, so the body outlives the line
    u32 body      = 0;
};

// Built on first use, as the job and variable tables are.
Vec<FuncEntry *> *g_funcs;

Vec<FuncEntry *> &funcs()
{
    if (!g_funcs) {
        g_funcs = heap_new<Vec<FuncEntry *>>();
        if (!g_funcs)
            panic("sh: out of memory");
    }
    return *g_funcs;
}

const FuncEntry *func_find(Str name)
{
    for (const FuncEntry *e : funcs())
        if (e->name.str() == name)
            return e;
    return nullptr;
}

bool func_define(Str name, const Tree *t, u32 body)
{
    for (FuncEntry *e : funcs())
        if (e->name.str() == name) {
            tree_hold(t);
            tree_release(e->t);
            e->t    = t;
            e->body = body;
            return true;
        }

    FuncEntry *e = heap_new<FuncEntry>();
    if (!e)
        return false;
    if (!e->name.assign(name) || !funcs().push(e)) {
        heap_delete(e);
        return false;
    }
    tree_hold(t);
    e->t    = t;
    e->body = body;
    return true;
}

// One line of the job table. It outlives the frame that started the job, so the
// command text is copied rather than viewed.
struct JobEntry {
    u32 id = 0;
    String cmd;
    Vec<u32> pids;
    bool running = true;
    i32 status   = 0;
};

// Built on first use: a namespace-scope Vec has a destructor, and nothing
// provides __cxa_atexit — the rule holds inside a process exactly as it does in
// the kernel.
Vec<JobEntry *> *g_jobs;
u32 g_next_id = 1;
u32 g_current = 0;

Vec<JobEntry *> &jobs()
{
    if (!g_jobs) {
        g_jobs = heap_new<Vec<JobEntry *>>();
        if (!g_jobs)
            panic("sh: out of memory");
    }
    return *g_jobs;
}

JobEntry *find_entry(u32 id)
{
    for (JobEntry *e : jobs())
        if (e->id == id)
            return e;
    return nullptr;
}

void drop_entry(u32 id)
{
    Vec<JobEntry *> &t = jobs();
    for (usize i = 0; i < t.size(); i++) {
        if (t[i]->id != id)
            continue;
        if (g_current == id)
            g_current = 0;
        heap_delete(t[i]);
        t.erase(i);
        return;
    }
}

// One raw word, running whatever substitutions it turns out to need. Defined
// below, next to the walk it has to reach, as call_func is.
Task<Result<void>> expand_one(const Ctx &cx, Str raw, Split split, Vec<Field> &out, ExpandErr &err);

Task<i32> call_func(Ctx &cx, const FuncEntry *fn, Args a);

// The walk a builtin is running inside, so that `.` and `eval` can reach it
// through job.h without Ctx leaving this file.
Ctx *g_ctx;

// Everything the pipeline holds. An assignment value and a redirection target
// stay one word; only an argv word may become several, or none — and only an
// argv word is globbed, since the other two are one field by definition.
Task<Result<void>> expand_all(const Ctx &cx, Run *r, ExpandErr &err)
{
    usize n = r->ncmds;
    Vec<Field> raw_fields;

    for (usize i = 0; i < n; i++) {
        if (!r->assign0.push(r->avals.size()))
            co_return Err(Error::NoMemory);
        Args a = r->t->assigns(r->cmd0 + i);
        for (usize k = 0; k < a.size(); k++) {
            Str w = a[k];
            Task<Result<void>> t =
                expand_one(cx, w.substr(w.find('=') + 1), Split::One, r->avals, err);
            CO_TRY_VOID(t ? co_await t : Err(Error::NoMemory));
        }
    }

    for (usize i = 0; i < n; i++) {
        if (!r->argv0.push(r->words.size()))
            co_return Err(Error::NoMemory);
        Args raw = r->t->args(r->cmd0 + i);
        for (usize k = 0; k < raw.size(); k++) {
            Task<Result<void>> t = expand_one(cx, raw[k], Split::Fields, raw_fields, err);
            CO_TRY_VOID(t ? co_await t : Err(Error::NoMemory));
        }
        Task<Result<void>> g = glob_fields(raw_fields, r->words);
        CO_TRY_VOID(g ? co_await g : Err(Error::NoMemory));
    }

    usize argvn = r->words.size();
    for (usize i = 0; i < n; i++)
        for (const Redirect &rd : r->t->redirects(r->cmd0 + i)) {
            Task<Result<void>> t = expand_one(cx, r->t->target(rd), Split::One, r->words, err);
            CO_TRY_VOID(t ? co_await t : Err(Error::NoMemory));
        }

    // The views come after the last append, as parse.h's freeze() does.
    if (!r->argv.reserve(argvn) || !r->targets.reserve(r->words.size() - argvn))
        co_return Err(Error::NoMemory);
    for (usize k = 0; k < r->words.size(); k++)
        (k < argvn ? r->argv : r->targets).push(r->words[k].text.str());
    co_return {};
}

// One stage's assignment prefix. With `undo` the displaced values are kept.
bool apply_assigns(Run *r, usize i, Vec<Saved> *undo, Str &bad_name)
{
    Args names = r->t->assigns(r->cmd0 + i);
    usize at   = r->assign0[i];

    for (usize k = 0; k < names.size(); k++) {
        Str w     = names[k];
        Str name  = w.substr(0, w.find('='));
        Str value = r->avals[at + k].text.str();
        bad_name  = name;

        if (undo) {
            Saved s;
            Str old;
            s.was_set = var_get(name, old);
            if (!s.name.assign(name) || (s.was_set && !s.value.assign(old)) || !undo->push(move(s)))
                return false;
        }
        if (!var_set(name, value))
            return false;
    }
    return true;
}

void undo_assigns(Vec<Saved> &undo)
{
    while (!undo.empty()) {
        Saved &s = undo.back();
        if (s.was_set)
            var_set(s.name.str(), s.value.str());
        else
            var_unset(s.name.str());
        undo.pop();
    }
}

// A diagnostic on the shell's own stderr.
Task<void> say(u32 fd, Str what)
{
    String line;
    if (line.assign("braam: ") && line.append(what) && line.push('\n'))
        if (Task<Result<void>> t = write_all(fd, line.str()))
            co_await t;
}

Task<void> say2(u32 fd, Str what, Str why)
{
    String line;
    if (line.assign("braam: ") && line.append(what) && line.append(": ") && line.append(why) &&
        line.push('\n'))
        if (Task<Result<void>> t = write_all(fd, line.str()))
            co_await t;
}

// What an expansion failure prints, whichever construct asked.
Task<void> say_expand(Error e, const ExpandErr &err)
{
    if (e != Error::Invalid)
        co_await say(SYS_STDERR, "out of memory");
    else if (err.name.empty())
        co_await say(SYS_STDERR, err.message);
    else
        co_await say2(SYS_STDERR, err.name, err.message);
}

u32 open_flags(Redir kind)
{
    switch (kind) {
    case Redir::In:
        return SYS_O_READ;
    case Redir::Out:
    case Redir::ErrOut:
        return SYS_O_WRITE | SYS_O_CREATE | SYS_O_TRUNC;
    case Redir::Append:
    case Redir::ErrAppend:
        return SYS_O_WRITE | SYS_O_CREATE | SYS_O_APPEND;
    }
    return SYS_O_READ;
}

// Whether a stage's descriptor is one of ours to close rather than one of the
// three we were handed.
bool ours(u32 fd)
{
    return fd >= SYS_FD_MIN;
}

Task<void> close_ours(u32 fd)
{
    if (ours(fd))
        co_await close_fd(fd);
}

// Every descriptor a stage holds that no child took. A builtin's are closed the
// moment it finishes, so that whatever reads the other end sees an end of
// input; a stage that never started leaves its to the sweep at the end.
Task<void> close_stage(const Stage &s)
{
    co_await close_ours(s.in);
    if (s.out != s.in)
        co_await close_ours(s.out);
    if (s.err != s.in && s.err != s.out)
        co_await close_ours(s.err);
}

// Taking the keyboard back, which can in principle be refused. A child's claim
// is the kernel's, on its process record, and that record outlives the child by
// as long as its last syscall server takes to unwind — so a program killed
// while holding the keys may in theory still hold them when the wait for it
// returns.
//
// It has not been seen to happen: the wait's own reply costs a step or two, and
// the servers unwind inside that. But nothing orders the two, and the cost of
// being wrong is a terminal that is simply dead — the next key_read is
// Err(Perm), the prompt never comes back, and there is no way left to say so.
// Eight tries at zero delay is a cheap thing to be sure about.
Task<Result<void>> retake_keys()
{
    for (usize i = 0; i < 8; i++) {
        if (Task<Result<Geometry>> t = keys_claim(true))
            if ((co_await t).is_ok())
                co_return {};
        if (Task<Result<void>> s = sleep_for(0))
            co_await s;
    }
    co_return Err(Error::Perm);
}

// Is this pid still a task the kernel knows about? /proc is how a process asks,
// and asking is the whole point: nothing here may park on a job that is still
// running, because the prompt has to come back either way.
Task<bool> alive(u32 pid)
{
    Buf<32> path;
    path.put("/proc/").put(pid);
    Task<Result<FileInfo>> t = stat_of(path.str());
    if (!t)
        co_return true;
    co_return (co_await t).is_ok();
}

} // namespace

usize jobs_count()
{
    return jobs().size();
}

bool jobs_at(usize i, JobInfo &out)
{
    Vec<JobEntry *> &t = jobs();
    if (i >= t.size())
        return false;
    const JobEntry *e = t[i];
    out = JobInfo{ e->id, e->pids.empty() ? 0 : e->pids[0], e->cmd.str(), e->running, e->status };
    return true;
}

bool jobs_find(u32 id, JobInfo &out)
{
    const JobEntry *e = find_entry(id);
    if (!e)
        return false;
    out = JobInfo{ e->id, e->pids.empty() ? 0 : e->pids[0], e->cmd.str(), e->running, e->status };
    return true;
}

u32 jobs_current()
{
    return g_current;
}

Task<Result<void>> jobs_kill(u32 id)
{
    JobEntry *e = find_entry(id);
    if (!e)
        co_return Err(Error::Invalid);
    if (e->running)
        for (u32 pid : e->pids)
            if (Task<Result<void>> t = kill_child(pid))
                co_await t;
    co_return {};
}

Task<Result<i32>> jobs_wait(u32 id, bool interactive)
{
    JobEntry *e = find_entry(id);
    if (!e)
        co_return Err(Error::Invalid);
    if (!e->running) {
        i32 s = e->status;
        drop_entry(id);
        co_return s;
    }

    // In front while we wait for it, so ^C reaches the job rather than us —
    // which is what `fg` means. The keyboard goes first, in run_line's order
    // and for its reason: a job that wants raw keys can only claim them once we
    // have let go.
    if (interactive) {
        if (Task<Result<Geometry>> t = keys_claim(false))
            co_await t;
        for (u32 pid : e->pids)
            if (Task<Result<void>> t = set_fg(pid))
                co_await t;
    }

    i32 status = 0;
    for (usize i = 0; i < e->pids.size(); i++) {
        Task<Result<Exited>> t = wait_child(e->pids[i]);
        if (!t)
            continue;
        Result<Exited> r = co_await t;
        if (r.is_ok() && i + 1 == e->pids.size())
            status = r.value().status;
    }

    if (interactive) {
        if (Task<Result<void>> t = set_fg(0))
            co_await t;
        if (Task<Result<void>> t = retake_keys())
            co_await t;
    }

    drop_entry(id);
    co_return status;
}

Task<void> jobs_report(u32 fd)
{
    Vec<JobEntry *> &t = jobs();
    for (usize i = 0; i < t.size();) {
        JobEntry *e = t[i];

        if (e->running) {
            bool any = false;
            for (u32 pid : e->pids)
                if (co_await alive(pid))
                    any = true;
            if (any) {
                i++;
                continue;
            }

            // Every stage has gone, so each wait answers at once and none of
            // them parks. Collecting is what frees the kernel's child slots.
            for (usize k = 0; k < e->pids.size(); k++)
                if (Task<Result<Exited>> w = wait_child(e->pids[k])) {
                    Result<Exited> r = co_await w;
                    if (r.is_ok() && k + 1 == e->pids.size())
                        e->status = r.value().status;
                }
            e->running = false;
        }

        Buf<96> line;
        line.put('[').put(e->id).put("] ");
        if (e->status == 0)
            line.put("done      ");
        else if (e->status == 130)
            line.put("interrupt ");
        else
            line.put("exit ").put(e->status).put("    ");
        line.put(e->cmd.str()).put('\n');

        u32 id = e->id;
        if (Task<Result<void>> w = write_all(fd, line.str()))
            co_await w;
        drop_entry(id);
    }
}

namespace {

// One pipeline: today's whole line, now the leaf of the walk below.
Task<i32> exec_pipeline(Ctx &cx, const Tree &tree, u32 node)
{
    const Tree::Node &nd = tree.node(node);

    Run *r = heap_new<Run>();
    if (!r) {
        co_await say(SYS_STDERR, "out of memory");
        co_return 1;
    }
    struct Drop {
        ~Drop() { heap_delete(r); }

        Run *r;
    } drop{ r };

    r->t     = &tree;
    r->cmd0  = nd.a;
    r->ncmds = nd.b - nd.a;

    usize n = r->ncmds;
    if (!r->stages.reserve(n)) {
        co_await say(SYS_STDERR, "out of memory");
        co_return 1;
    }
    for (usize i = 0; i < n; i++)
        r->stages.push(Stage{});

    // Expansion, before anything is opened or spawned.
    ExpandErr xerr;
    Task<Result<void>> xt = expand_all(cx, r, xerr);
    if (Result<void> e = xt ? co_await xt : Err(Error::NoMemory); e.is_err()) {
        // A ^C caught mid-glob abandons the line, as a stage reporting 130 does.
        if (e.error() == Error::Cancelled) {
            var_status(130);
            co_return 130;
        }
        if (e.error() != Error::Invalid)
            co_await say(SYS_STDERR, "out of memory");
        else if (xerr.name.empty())
            co_await say(SYS_STDERR, xerr.message);
        else
            co_await say2(SYS_STDERR, xerr.name, xerr.message);
        var_status(1);
        co_return 1;
    }

    // Redirections first. Refusing here rather than at the first write is what
    // stops a command running and producing side effects before its
    // redirection turns out to be impossible.
    i32 bad  = 0;
    usize at = 0; // the next target, in the order they were expanded above
    for (usize i = 0; i < n && !bad; i++) {
        for (const Redirect &rd : r->t->redirects(r->cmd0 + i)) {
            Str path            = r->targets[at++];
            Task<Result<i32>> t = open_at(path, open_flags(rd.kind));
            Result<i32> fd      = t ? co_await t : Err(Error::NoMemory);
            if (fd.is_err()) {
                co_await say2(SYS_STDERR, path, error_name(fd.error()));
                bad = 1;
                break;
            }

            // A second redirection of the same stream replaces the first, which
            // is what `> a > b` means; the earlier file is closed first.
            u32 *slot = rd.kind == Redir::In ? &r->stages[i].in
                        : (rd.kind == Redir::ErrOut || rd.kind == Redir::ErrAppend)
                            ? &r->stages[i].err
                            : &r->stages[i].out;
            if (ours(*slot))
                co_await close_fd(*slot);
            *slot = u32(fd.value());
        }
    }

    // The pipes. Each end is moved into exactly one child, or borrowed by one
    // builtin and closed here — one end, one owner (Concept.md §4.3).
    for (usize i = 0; i + 1 < n && !bad; i++) {
        Task<Result<Piped>> t = make_pipe();
        Result<Piped> p       = t ? co_await t : Err(Error::NoMemory);
        if (p.is_err()) {
            co_await say(SYS_STDERR, "out of memory");
            bad = 1;
            break;
        }
        // A redirection displaces the pipe for that stream, but the pipe is
        // still made and still closed — so `a > f | b` gives b a clean end of
        // input rather than a wait that never finishes.
        if (r->stages[i].out == SYS_STDOUT)
            r->stages[i].out = u32(p.value().w);
        else if (!r->spare.push(u32(p.value().w)))
            bad = 1;
        if (r->stages[i + 1].in == SYS_STDIN)
            r->stages[i + 1].in = u32(p.value().r);
        else if (!r->spare.push(u32(p.value().r)))
            bad = 1;
    }

    // The capture pipe, when a `$(…)` is running this pipeline. Only the last
    // stage can still hold SYS_STDOUT; `$(cmd > f)` displaced it, and then
    // nothing is captured, which is what every shell does.
    bool captured = false;
    for (usize i = 0; i < n && cx.capture && !bad; i++) {
        if (r->stages[i].out != SYS_STDOUT)
            continue;
        Task<Result<Piped>> t = make_pipe();
        Result<Piped> p       = t ? co_await t : Err(Error::NoMemory);
        if (p.is_err()) {
            co_await say(SYS_STDERR, "out of memory");
            bad = 1;
            break;
        }
        r->stages[i].out = u32(p.value().w);
        r->capture_r     = u32(p.value().r);
        captured         = true;
    }

    // A stage may have no command word: `x=1` is one, and so is a word that
    // expanded to nothing. Its redirections are made, its assignments stay.
    for (usize i = 0; i < n && !bad; i++) {
        Args a = r->args(i);
        if (a.size()) {
            // A function first, then a builtin, then /bin.
            r->stages[i].fn = func_find(a[0]);
            if (!r->stages[i].fn)
                r->stages[i].b = builtin_find(a[0]);
            // Its body runs in this turn with the shell's own stdio, which S7
            // is what threads through the walk.
            else if (n > 1 || r->t->redirects(r->cmd0 + i).size()) {
                co_await say2(SYS_STDERR, a[0], "a function cannot be piped or redirected yet");
                bad = 1;
            }
            continue;
        }
        Str who;
        if (!apply_assigns(r, i, nullptr, who)) {
            co_await say2(SYS_STDERR, who, "cannot be set");
            bad = 1;
        }
    }

    // The keyboard goes back *before* anything is spawned. A child is a
    // scheduler job that runs as soon as this shell next parks, and a
    // full-screen program claims the keys in its very first step — so handing
    // them over afterwards is a race the child loses, and `less: no keyboard`
    // is what that looks like.
    bool bg     = nd.background;
    bool handed = false;
    if (!bad && !bg && cx.interactive) {
        if (Task<Result<Geometry>> t = keys_claim(false))
            co_await t;
        handed = true;
    }

    // The program stages, all of them before anything runs: a builtin below
    // may write into a pipe whose reader is one of these.
    for (usize i = 0; i < n && !bad; i++) {
        Stage &s = r->stages[i];
        if (s.b || s.fn || !r->args(i).size())
            continue;
        // An assignment prefix has nowhere to go: a child gets three
        // descriptors, an argv and a cwd, and no environment (§4.3).
        Task<Result<u32>> t = spawn(r->args(i), ChildIo{ s.in, s.out, s.err });
        Result<u32> pid     = t ? co_await t : Err(Error::NoMemory);
        if (pid.is_err()) {
            // A stale binary is its own answer: the file is there and is a
            // program, and rebuilding is the repair. Still 126, which is what
            // "found but would not run" means.
            // A literal per branch, not one conditional expression: Str's
            // length comes from __builtin_strlen, which folds only when the
            // pointer is a constant — otherwise this is a call to strlen and
            // there is no libc to link (Concept.md §C.3).
            Str why("not executable");
            if (pid.error() == Error::NotFound)
                why = "not found";
            else if (pid.error() == Error::Unsupported)
                why = "built for another process ABI";
            co_await say2(SYS_STDERR, r->args(i)[0], why);
            bad = pid.error() == Error::NotFound ? 127 : 126;
            break;
        }
        s.pid   = pid.value();
        s.moved = true; // the three descriptors are the child's now
    }

    // In front once they exist, so ^C reaches them rather than us. Nothing is
    // in front between the release above and here, which is the one window in
    // which a ^C is dropped rather than delivered — a few steps wide.
    if (handed)
        for (const Stage &s : r->stages)
            if (s.pid)
                if (Task<Result<void>> t = set_fg(s.pid))
                    co_await t;

    // The builtins, in pipeline order, each to completion. Nothing in this
    // process can wait for a sibling task, so they run in turn rather than
    // alongside — which is why a builtin writes its output once (builtin.h).
    i32 status = bad;
    for (usize i = 0; i < n && !bad; i++) {
        Stage &s = r->stages[i];
        if (!s.b && !s.fn)
            continue;
        // The prefix is this command's for its turn and then goes back.
        Str who;
        if (!apply_assigns(r, i, &r->saved, who)) {
            co_await say2(SYS_STDERR, who, "cannot be set");
            s.status = 1;
        } else if (s.fn) {
            Task<i32> t = call_func(cx, s.fn, r->args(i));
            s.status    = t ? co_await t : 1;
        } else {
            Ctx *outer = g_ctx;
            g_ctx      = &cx;
            if (Task<i32> t = s.b->run(r->args(i), ShIo{ s.in, s.out, s.err }))
                s.status = co_await t;
            else
                s.status = 1;
            g_ctx = outer;
        }
        undo_assigns(r->saved);
        // Closed the moment it is done, so whatever reads the other end sees
        // an end of input rather than waiting for a writer that has finished.
        co_await close_stage(s);
        s.in = s.out = s.err = SYS_STDIN;
        if (i + 1 == n)
            status = s.status;
    }

    // Drained before the wait, not after: a child parked in a full pipe has
    // not exited, so waiting first is a deadlock only ^C could break
    // (../cmd/watch.cpp says the same). By here the write end has gone into a
    // child, or was closed with the builtin that held it, so this is the only
    // end left and the read ends when the writer does.
    if (captured && !bad) {
        for (;;) {
            Task<Result<String>> t = read_chunk(r->capture_r);
            Result<String> got     = t ? co_await t : Err(Error::NoMemory);
            if (got.is_err())
                break;
            if (!cx.capture->append(got.value().str())) {
                co_await say(SYS_STDERR, "out of memory");
                status = 1;
                break;
            }
        }
    }
    if (captured)
        co_await close_fd(r->capture_r);

    // Collecting every child. A cancelled one answers 130 of its own accord,
    // so ^C needs nothing here: it went to the stages, not to us.
    for (usize i = 0; i < n && !bg; i++) {
        Stage &s = r->stages[i];
        if (!s.pid)
            continue;
        Task<Result<Exited>> t = wait_child(s.pid);
        Result<Exited> e       = t ? co_await t : Err(Error::NoMemory);
        s.status               = e.is_ok() ? e.value().status : 1;
        if (i + 1 == n)
            status = s.status;
    }

    if (handed) {
        if (Task<Result<void>> t = set_fg(0))
            co_await t;
        if (Task<Result<void>> t = retake_keys())
            co_await t;
    }

    // Backgrounded: the job goes in the table and the shell comes straight back
    // to the prompt. Its stdin is at end of input from the start, since the
    // keyboard belongs to whatever is in front.
    if (bg && !bad) {
        JobEntry *e = heap_new<JobEntry>();
        if (e && e->cmd.assign(tree.text(node)) && jobs().push(e)) {
            e->id = g_next_id++;
            for (const Stage &s : r->stages)
                if (s.pid)
                    e->pids.push(s.pid);
            g_current = e->id;
            var_last_bg(e->pids.empty() ? 0 : e->pids[0]);

            Buf<32> note;
            note.put('[').put(e->id).put("] ").put(e->pids.empty() ? 0 : e->pids[0]).put('\n');
            if (Task<Result<void>> t = write_all(SYS_STDERR, note.str()))
                co_await t;
        } else {
            heap_delete(e);
            co_await say(SYS_STDERR, "out of memory");
            status = 1;
        }
    }

    // Whatever nobody took. A stage that was spawned owns its three and must
    // not have them closed under it.
    for (const Stage &s : r->stages)
        if (!s.moved)
            co_await close_stage(s);
    for (u32 fd : r->spare)
        co_await close_fd(fd);

    // `exit` ran in our turn, so the flag is consumed here and put back by
    // run_line; without that the rest of the list would still run. 130 is this
    // shell's SIGINT (job.h), and it stops the list for the same reason.
    i32 want = 0;
    if (shell_exit_wanted(want)) {
        cx.flow        = Flow::Exit;
        cx.exit_status = want;
    } else if (g_return) {
        // Outside a function or a sourced file it is a no-op, as `break` is
        // outside a loop.
        if (cx.frames) {
            cx.flow        = Flow::Return;
            cx.exit_status = g_return_status;
        }
        g_return = false;
    } else if (g_break) {
        // Outside a loop it is a silent no-op, as in v7.
        if (cx.loops) {
            cx.flow   = g_break_cont ? Flow::Continue : Flow::Break;
            cx.breaks = g_break;
        }
        g_break = 0;
    } else if (!bg && status == 130) {
        cx.flow = Flow::Interrupt;
    }

    var_status(status);
    co_return status;
}

// What a loop does with the flow its body left; false stops it. Exit and
// Interrupt pass through, which is what makes one ^C enough.
bool loop_step(Ctx &cx)
{
    if (cx.flow != Flow::Break && cx.flow != Flow::Continue)
        return cx.flow == Flow::Normal;

    bool cont = cx.flow == Flow::Continue;
    if (cx.breaks > 1) {
        cx.breaks--; // an outer loop takes the rest
        return false;
    }
    cx.flow   = Flow::Normal;
    cx.breaks = 0;
    return cont;
}

// The walk, which every construct below runs its parts through.
Task<i32> exec_node(Ctx &cx, const Tree &tree, u32 n);

// One substitution's output, remembered under where it began in the word so a
// retry finds it rather than running the command twice.
struct Sub {
    usize at = 0;
    String out;
};

bool cb_sub(void *ctx, usize at, Str, Str &out)
{
    for (const Sub &s : *static_cast<Vec<Sub> *>(ctx))
        if (s.at == at) {
            out = s.out.str();
            return true;
        }
    return false;
}

// A substitution's command. Parsed and run here rather than through run_line,
// which clears a pending `break` and would let `$(exit)` end the shell.
Task<Result<void>> run_capture(const Ctx &cx, Str command, bool ticked, ExpandErr &err, String &out)
{
    String text;
    if (ticked) {
        // Inside backticks a backslash stands for itself except in front of
        // one of these three.
        for (usize i = 0; i < command.size(); i++) {
            if (command[i] == '\\' && i + 1 < command.size() &&
                (command[i + 1] == '`' || command[i + 1] == '\\' || command[i + 1] == '$'))
                i++;
            if (!text.push(command[i]))
                co_return Err(Error::NoMemory);
        }
    } else if (!text.assign(command)) {
        co_return Err(Error::NoMemory);
    }

    // Declared after `text`, so the tree that views it is destroyed first.
    struct Drop {
        ~Drop()
        {
            heap_delete(t);
            heap_delete(cx);
        }

        Tree *t;
        Ctx *cx;
    } own{ heap_new<Tree>(), heap_new<Ctx>() };

    if (!own.t || !own.cx)
        co_return Err(Error::NoMemory);

    ParseErr perr;
    if (parse(text.str(), *own.t, perr).is_err()) {
        err.name    = Str();
        err.message = perr.message;
        co_return Err(Error::Invalid);
    }
    if (own.t->root() == 0)
        co_return {};

    own.cx->line        = text.str();
    own.cx->interactive = cx.interactive;
    own.cx->depth       = cx.depth; // so `$($($(…)))` is bounded like any nest
    own.cx->capture     = &out;

    if (Task<i32> t = exec_node(*own.cx, *own.t, own.t->root()))
        co_await t;

    // A ^C inside the substitution abandons the word, and the line with it.
    if (own.cx->flow == Flow::Interrupt)
        co_return Err(Error::Cancelled);
    co_return {};
}

// A function body, run on the caller's Ctx so that a capture, the keyboard and
// the depth bound all reach it. The tree is held for the call: a body that
// redefines its own name must not free what it is running.
Task<i32> call_func(Ctx &cx, const FuncEntry *fn, Args a)
{
    Vec<String> next;
    String zero;
    if (!zero.assign(args_at(0)) || !next.reserve(a.size()) || !next.push(move(zero))) {
        co_await say(SYS_STDERR, "out of memory");
        co_return 1;
    }
    for (usize i = 1; i < a.size(); i++) {
        String s;
        if (!s.assign(a[i]) || !next.push(move(s))) {
            co_await say(SYS_STDERR, "out of memory");
            co_return 1;
        }
    }

    const Tree *t = fn->t;
    u32 body      = fn->body;
    tree_hold(t);

    Vec<String> saved = args_swap(move(next));
    cx.frames++;

    Task<i32> step = exec_node(cx, *t, body);
    i32 status     = step ? co_await step : 1;

    cx.frames--;
    args_swap(move(saved));
    if (cx.flow == Flow::Return) {
        cx.flow = Flow::Normal;
        status  = cx.exit_status;
        var_status(status);
    }
    tree_release(t);
    co_return status;
}

// `.` and `eval` are one thing twice: a text parsed into a tree of its own and
// run in the walk already in progress. A tree that defined something outlives
// this call, so it owns its text and carries the reference.
Task<i32> run_text(Ctx &cx, String text)
{
    struct Drop {
        ~Drop() { tree_release(t); }

        Tree *t;
    } own{ heap_new<Tree>() };

    if (!own.t) {
        co_await say(SYS_STDERR, "out of memory");
        co_return 1;
    }
    tree_hold(own.t);

    ParseErr perr;
    if (parse(text.str(), *own.t, perr).is_err()) {
        co_await say(SYS_STDERR, perr.message);
        co_return 2;
    }
    if (own.t->defines() && own.t->own_source().is_err()) {
        co_await say(SYS_STDERR, "out of memory");
        co_return 1;
    }
    if (own.t->root() == 0)
        co_return 0;

    cx.frames++;
    Task<i32> step = exec_node(cx, *own.t, own.t->root());
    i32 status     = step ? co_await step : 1;
    cx.frames--;

    if (cx.flow == Flow::Return) {
        cx.flow = Flow::Normal;
        status  = cx.exit_status;
        var_status(status);
    }
    co_return status;
}

// The expander is pure and cannot await, so it abandons the word the first
// time it meets a substitution whose output nobody has. Running it and
// expanding again is idempotent: `${x=$(a)}` gives up before the assignment,
// so the retry takes the same branch and the command runs exactly once.
Task<Result<void>> expand_one(const Ctx &cx, Str raw, Split split, Vec<Field> &out, ExpandErr &err)
{
    Vec<Sub> memo;
    Vars v       = shell_vars();
    v.ctx        = &memo;
    v.substitute = cb_sub;

    Vec<Field> got;
    for (;;) {
        got.clear();
        Result<void> e = expand_word(raw, v, split, got, err);
        if (e.is_ok()) {
            for (Field &f : got)
                if (!out.push(move(f)))
                    co_return Err(Error::NoMemory);
            co_return {};
        }
        if (e.error() != Error::Again)
            co_return Err(e.error());

        Sub s;
        s.at                 = err.at;
        Task<Result<void>> t = run_capture(cx, err.command, err.ticked, err, s.out);
        CO_TRY_VOID(t ? co_await t : Err(Error::NoMemory));
        if (!memo.push(move(s)))
            co_return Err(Error::NoMemory);
    }
}

Task<i32> exec_if(Ctx &cx, const Tree &tree, u32 n)
{
    const Tree::Node &nd = tree.node(n);

    Task<i32> step;
    i32 status = 0; // no branch taken is 0, which is POSIX rather than v7
    u32 k      = 0;

    bool took = false;
    for (k = 0; k < nd.b && !took; k++) {
        step   = exec_node(cx, tree, tree.kid(nd.a + 2 * k));
        status = step ? co_await step : 1;
        if (cx.flow != Flow::Normal)
            co_return status;
        if (status != 0)
            continue;
        step   = exec_node(cx, tree, tree.kid(nd.a + 2 * k + 1));
        status = step ? co_await step : 1;
        took   = true;
    }

    if (!took) {
        status = 0;
        if (nd.c) {
            step   = exec_node(cx, tree, nd.c);
            status = step ? co_await step : 1;
        }
        // Nothing ran, so nothing published a status: `if false; then a; fi`
        // is 0 and not the condition's, which is POSIX rather than v7.
        else
            var_status(status);
    }
    co_return status;
}

// `while` and `until` differ by one comparison, as they do in v7.
Task<i32> exec_loop(Ctx &cx, const Tree &tree, u32 n)
{
    const Tree::Node &nd = tree.node(n);
    bool until           = nd.kind == Tree::Kind::Until;

    Task<i32> step;
    i32 status = 0; // a body that never runs is 0
    i32 cond   = 0;
    bool ran   = false;

    cx.loops++;
    for (;;) {
        step = exec_node(cx, tree, nd.a);
        cond = step ? co_await step : 1;
        if (cx.flow != Flow::Normal) {
            loop_step(cx);
            break;
        }
        if ((cond == 0) == until)
            break;

        step   = exec_node(cx, tree, nd.b);
        status = step ? co_await step : 1;
        if (!loop_step(cx))
            break;
        ran = true;
    }
    cx.loops--;
    if (!ran)
        var_status(status); // a body that never ran is 0, not the condition's
    co_return status;
}

Task<i32> exec_for(Ctx &cx, const Tree &tree, u32 n)
{
    const Tree::Node &nd = tree.node(n);
    Args names           = tree.args(nd.b); // the name, then the words
    Str name             = names[0];

    // Owned copies, so a `set` or `shift` in the body cannot move them —
    // v7 takes a reference on the parameter block for the same reason.
    Vec<String> items;
    if (nd.c) {
        Vec<Field> raw_fields;
        Vec<Field> fields;
        ExpandErr err;
        for (usize i = 1; i < names.size(); i++) {
            Task<Result<void>> t = expand_one(cx, names[i], Split::Fields, raw_fields, err);
            if (Result<void> e = t ? co_await t : Err(Error::NoMemory); e.is_err()) {
                if (e.error() == Error::Cancelled) {
                    var_status(130);
                    co_return 130;
                }
                if (Task<void> d = say_expand(e.error(), err))
                    co_await d;
                var_status(1);
                co_return 1;
            }
        }
        Task<Result<void>> g = glob_fields(raw_fields, fields);
        if (Result<void> e = g ? co_await g : Err(Error::NoMemory); e.is_err()) {
            if (e.error() == Error::Cancelled) {
                var_status(130);
                co_return 130;
            }
            co_await say(SYS_STDERR, "out of memory");
            co_return 1;
        }
        if (!items.reserve(fields.size())) {
            co_await say(SYS_STDERR, "out of memory");
            co_return 1;
        }
        for (Field &f : fields)
            items.push(move(f.text));
    } else {
        // `for i; do` walks the positional parameters.
        if (!items.reserve(args_count())) {
            co_await say(SYS_STDERR, "out of memory");
            co_return 1;
        }
        for (usize k = 1; k <= args_count(); k++) {
            String v;
            if (!v.assign(args_at(k))) {
                co_await say(SYS_STDERR, "out of memory");
                co_return 1;
            }
            items.push(move(v));
        }
    }

    Task<i32> step;
    i32 status = 0;
    usize k    = 0;
    bool ran   = false;

    cx.loops++;
    for (k = 0; k < items.size(); k++) {
        if (!var_set(name, items[k].str())) {
            co_await say2(SYS_STDERR, name, "cannot be set");
            status = 1;
            break;
        }
        step   = exec_node(cx, tree, nd.a);
        status = step ? co_await step : 1;
        if (!loop_step(cx))
            break;
        ran = true;
    }
    cx.loops--;
    if (!ran)
        var_status(status);
    co_return status;
}

// The word and each pattern are one field however they expand, and a pattern
// keeps its quoting mask — which is what makes `'a*'` match a literal star.
// Not a loop, so it never touches cx.loops: a `break` in an arm belongs to
// whatever loop is above.
Task<i32> exec_case(Ctx &cx, const Tree &tree, u32 n)
{
    const Tree::Node &nd = tree.node(n);

    Vec<Field> subject;
    ExpandErr err;
    Task<Result<void>> st = expand_one(cx, tree.args(nd.c)[0], Split::One, subject, err);
    if (Result<void> e = st ? co_await st : Err(Error::NoMemory); e.is_err()) {
        if (e.error() == Error::Cancelled) {
            var_status(130);
            co_return 130;
        }
        if (Task<void> t = say_expand(e.error(), err))
            co_await t;
        var_status(1);
        co_return 1;
    }

    Task<i32> step;
    i32 status = 0;
    u32 k      = 0;
    bool took  = false;

    for (k = 0; k < nd.b; k++) {
        Args pats = tree.args(tree.kid(nd.a + 2 * k));
        for (usize p = 0; p < pats.size() && !took; p++) {
            Vec<Field> pat;
            Task<Result<void>> pt = expand_one(cx, pats[p], Split::One, pat, err);
            if (Result<void> e = pt ? co_await pt : Err(Error::NoMemory); e.is_err()) {
                if (e.error() == Error::Cancelled) {
                    var_status(130);
                    co_return 130;
                }
                if (Task<void> t = say_expand(e.error(), err))
                    co_await t;
                var_status(1);
                co_return 1;
            }
            took = glob_match(pat[0].text.str(), Bytes(pat[0].mark.data(), pat[0].mark.size()),
                              subject[0].text.str());
        }
        if (!took)
            continue;
        step   = exec_node(cx, tree, tree.kid(nd.a + 2 * k + 1));
        status = step ? co_await step : 1;
        break;
    }

    // Nothing ran, so nothing published a status — as `if` with no branch.
    if (!took)
        var_status(0);
    co_return status;
}

// The walk. Only this recurses, so only this counts depth.
Task<i32> exec_node(Ctx &cx, const Tree &tree, u32 n)
{
    if (cx.depth >= Ctx::MAX_DEPTH) {
        co_await say(SYS_STDERR, "too deeply nested");
        cx.flow        = Flow::Exit;
        cx.exit_status = 2;
        co_return 2;
    }
    cx.depth++;
    struct Depth {
        ~Depth() { cx.depth--; }

        Ctx &cx;
    } guard{ cx };

    const Tree::Node &nd = tree.node(n);

    // One slot each, reused by every arm: a frame gets a slot per local that
    // is live across a suspend.
    Task<i32> step;
    i32 status = 0;
    u32 k      = 0;

    switch (nd.kind) {
    case Tree::Kind::Nop:
        break;

    case Tree::Kind::Pipe:
        step   = exec_pipeline(cx, tree, n);
        status = step ? co_await step : 1;
        break;

    case Tree::Kind::Seq:
        for (k = 0; k < nd.b; k++) {
            step   = exec_node(cx, tree, tree.kid(nd.a + k));
            status = step ? co_await step : 1;
            if (cx.flow != Flow::Normal)
                break;
        }
        break;

    case Tree::Kind::AndOr:
        for (k = 0; k < nd.b; k++) {
            // `&&` runs its right side after a 0 and `||` after anything
            // else. Skipped rather than stopped: the status carries down the
            // chain, so `false && a || b` still reaches b.
            if (k && (status == 0) != (tree.op(nd.c + k - 1) == Tree::Op::And))
                continue;
            step   = exec_node(cx, tree, tree.kid(nd.a + k));
            status = step ? co_await step : 1;
            if (cx.flow != Flow::Normal)
                break;
        }
        break;

    case Tree::Kind::Not:
        step   = exec_node(cx, tree, nd.a);
        status = step ? co_await step : 1;
        if (cx.flow == Flow::Normal) {
            status = status == 0 ? 1 : 0;
            var_status(status);
        }
        break;

    case Tree::Kind::Group:
        step   = exec_node(cx, tree, nd.a);
        status = step ? co_await step : 1;
        break;

    case Tree::Kind::If:
        step   = exec_if(cx, tree, n);
        status = step ? co_await step : 1;
        break;

    case Tree::Kind::While:
    case Tree::Kind::Until:
        step   = exec_loop(cx, tree, n);
        status = step ? co_await step : 1;
        break;

    case Tree::Kind::For:
        step   = exec_for(cx, tree, n);
        status = step ? co_await step : 1;
        break;

    case Tree::Kind::Case:
        step   = exec_case(cx, tree, n);
        status = step ? co_await step : 1;
        break;

    case Tree::Kind::FuncDef:
        // The tree the body is a node of takes a reference here, which is what
        // lets the definition outlive the line.
        if (!func_define(tree.args(nd.b)[0], &tree, nd.a)) {
            co_await say(SYS_STDERR, "out of memory");
            status = 1;
        }
        var_status(status);
        break;
    }
    co_return status;
}

} // namespace

void loop_break(u32 levels, bool cont)
{
    g_break      = levels;
    g_break_cont = cont;
}

void func_return(i32 status)
{
    g_return        = true;
    g_return_status = status;
}

bool func_unset(Str name)
{
    Vec<FuncEntry *> &t = funcs();
    for (usize i = 0; i < t.size(); i++) {
        if (t[i]->name.str() != name)
            continue;
        tree_release(t[i]->t);
        heap_delete(t[i]);
        t.erase(i);
        return true;
    }
    return false;
}

Task<i32> sh_source(Str path, ShIo io)
{
    if (!g_ctx) {
        co_await write_all(io.err, "braam: . outside a command\n");
        co_return 1;
    }

    Result<String> text = Err(Error::NoMemory);
    if (Task<Result<String>> t = read_file(path))
        text = co_await t;
    if (text.is_err()) {
        if (Task<void> e = errln("braam", path, text.error()))
            co_await e;
        co_return text.error() == Error::NotFound ? 127 : 1;
    }

    Task<i32> t = run_text(*g_ctx, move(text.value()));
    co_return t ? co_await t : 1;
}

Task<i32> sh_eval(Args args, ShIo io)
{
    if (!g_ctx) {
        co_await write_all(io.err, "braam: eval outside a command\n");
        co_return 1;
    }

    // The arguments are joined with one space, which is what makes
    // `eval echo a b` and `eval 'echo a b'` the same command.
    String text;
    for (usize i = 1; i < args.size(); i++)
        if ((i > 1 && !text.push(' ')) || !text.append(args[i]))
            co_return 1;
    if (text.empty())
        co_return 0;

    Task<i32> t = run_text(*g_ctx, move(text));
    co_return t ? co_await t : 1;
}

bool line_incomplete(Str text)
{
    Tree t;
    ParseErr err;
    return parse(text, t, err).is_err() && err.more;
}

Task<i32> run_line(Str line, bool interactive)
{
    // Two blocks per line, and neither in this frame: the tree outlives every
    // pipeline under it, and Ctx grows with every stage still to come.
    struct Drop {
        ~Drop()
        {
            tree_release(t);
            heap_delete(cx);
        }

        Tree *t;
        Ctx *cx;
    } own{ heap_new<Tree>(), heap_new<Ctx>() };

    if (!own.t || !own.cx) {
        co_await say(SYS_STDERR, "out of memory");
        co_return 1;
    }
    tree_hold(own.t);

    ParseErr perr;
    if (parse(line, *own.t, perr).is_err()) {
        co_await say(SYS_STDERR, perr.message);
        co_return 2;
    }
    // A definition pins the tree past this line, so it takes its text with it.
    if (own.t->defines() && own.t->own_source().is_err()) {
        co_await say(SYS_STDERR, "out of memory");
        co_return 1;
    }
    if (own.t->root() == 0)
        co_return 0;

    own.cx->line        = line;
    own.cx->interactive = interactive;

    Task<i32> t = exec_node(*own.cx, *own.t, own.t->root());
    i32 status  = t ? co_await t : 1;

    if (own.cx->flow == Flow::Exit)
        shell_exit(own.cx->exit_status);
    // `break 9` in one loop must not skip whatever comes next, which is what
    // v7 does with its own leftover.
    g_break  = 0;
    g_return = false;
    co_return status;
}

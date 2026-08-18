#include "var.h"

#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/host.h"

namespace {

// Built on first use: a namespace-scope Vec has a destructor, and nothing
// provides __cxa_atexit — the same rule the job table follows.
Vec<VarEntry *> *g_vars;
Vec<String> *g_args; // $0 at [0], the positional parameters after it

u32 g_pid;
i32 g_status;
u32 g_bg;

// The numeric specials, formatted on demand. One buffer is enough: the
// expander copies a value before it looks the next one up.
char g_num[16];

Vec<VarEntry *> &vars()
{
    if (!g_vars) {
        g_vars = heap_new<Vec<VarEntry *>>();
        if (!g_vars)
            panic("sh: out of memory");
    }
    return *g_vars;
}

Vec<String> &argv()
{
    if (!g_args) {
        g_args = heap_new<Vec<String>>();
        if (!g_args)
            panic("sh: out of memory");
    }
    return *g_args;
}

// A linear scan: a HashMap's keys would view Strings this table owns and moves.
VarEntry *find(Str name)
{
    for (VarEntry *e : vars())
        if (e->name.str() == name)
            return e;
    return nullptr;
}

VarEntry *make(Str name)
{
    VarEntry *e = find(name);
    if (e)
        return e;
    e = heap_new<VarEntry>();
    if (!e)
        return nullptr;
    if (!e->name.assign(name) || !vars().push(e)) {
        heap_delete(e);
        return nullptr;
    }
    return e;
}

Str keep_num(Str s)
{
    usize n = s.size() < sizeof(g_num) ? s.size() : sizeof(g_num);
    for (usize i = 0; i < n; i++)
        g_num[i] = s[i];
    return Str(g_num, n);
}

bool cb_look(void *, Str name, Str &value)
{
    if (name.size() == 1) {
        Buf<16> b;
        switch (name[0]) {
        case '?':
            value = keep_num(b.put(g_status).str());
            return true;
        case '$':
            value = keep_num(b.put(g_pid).str());
            return true;
        case '!':
            value = keep_num(b.put(g_bg).str());
            return true;
        case '#':
            value = keep_num(b.put(u32(args_count())).str());
            return true;
        default:
            break;
        }
    }
    return var_get(name, value);
}

bool cb_set(void *, Str name, Str value)
{
    return var_set(name, value);
}

usize cb_count(void *)
{
    return args_count();
}

Str cb_at(void *, usize i)
{
    return args_at(i);
}

} // namespace

bool var_set(Str name, Str value)
{
    VarEntry *e = find(name);
    if (e && e->readonly)
        return false;
    if (!e)
        e = make(name);
    if (!e || !e->value.assign(value))
        return false;
    e->valued = true;
    return true;
}

bool var_get(Str name, Str &value)
{
    VarEntry *e = find(name);
    if (!e || !e->valued)
        return false;
    value = e->value.str();
    return true;
}

bool var_unset(Str name)
{
    Vec<VarEntry *> &t = vars();
    for (usize i = 0; i < t.size(); i++) {
        if (t[i]->name.str() != name)
            continue;
        if (t[i]->readonly)
            return false;
        heap_delete(t[i]);
        t.erase(i);
        return true;
    }
    return true;
}

bool var_mark(Str name, bool exported, bool readonly)
{
    VarEntry *e = make(name);
    if (!e)
        return false;
    e->exported |= exported;
    e->readonly |= readonly;
    return true;
}

usize var_count()
{
    return vars().size();
}

const VarEntry *var_at(usize i)
{
    return i < vars().size() ? vars()[i] : nullptr;
}

bool args_set(Args a)
{
    Vec<String> &v = argv();

    Vec<String> next;
    if (!next.reserve(a.size() + 1))
        return false;

    String zero;
    if (!v.empty() && !zero.assign(v[0].str()))
        return false;
    next.push(move(zero));

    for (usize i = 0; i < a.size(); i++) {
        String s;
        if (!s.assign(a[i]) || !next.push(move(s)))
            return false;
    }

    v = move(next);
    return true;
}

bool args_shift(usize n)
{
    Vec<String> &v = argv();
    if (n == 0)
        return true;
    if (v.size() < n + 1)
        return false;
    v.erase(1, n);
    return true;
}

usize args_count()
{
    Vec<String> &v = argv();
    return v.empty() ? 0 : v.size() - 1;
}

Str args_at(usize i)
{
    Vec<String> &v = argv();
    return i < v.size() ? v[i].str() : Str();
}

bool var_init(u32 pid, Str name0)
{
    g_pid = pid;

    Vec<String> &v = argv();
    if (!v.empty())
        return true;
    String zero;
    return zero.assign(name0) && v.push(move(zero));
}

void var_status(i32 s)
{
    g_status = s;
}

void var_last_bg(u32 pid)
{
    g_bg = pid;
}

const Vars &shell_vars()
{
    // Constant-initialised: no guard variable, no __cxa_atexit (§C.3).
    static constexpr Vars V = { nullptr, cb_look, cb_set, cb_count, cb_at };
    return V;
}

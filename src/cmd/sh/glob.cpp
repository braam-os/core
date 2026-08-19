#include "glob.h"

#include "kernel/alloc.h"
#include "kernel/traits.h"
#include "match.h"
#include "proc/io.h"

namespace {

// The walk's state, off the coroutine frame: a frame past 512 bytes costs a
// whole 64 KiB span.
struct Walk {
    Vec<String> at;   // the prefixes matched so far, each ending in '/' or empty
    Vec<String> next; // what this component makes of them
    Vec<DirEntry> ents;
};

Bytes mask_of(const Field &f)
{
    return Bytes(f.mark.data(), f.mark.size());
}

bool unmarked_slash(const Field &f, usize i)
{
    return f.text[i] == '/' && (i >= f.mark.size() || f.mark[i] == 0);
}

// A prefix as a path to list: "" is the cwd, and the trailing '/' goes.
Str dir_of(const String &p)
{
    if (p.empty())
        return ".";
    if (p.size() == 1)
        return p.str();
    return p.str().substr(0, p.size() - 1);
}

bool emit(Vec<Field> &out, Str name)
{
    Field f;
    if (!f.text.assign(name) || !f.mark.reserve(name.size()))
        return false;
    // A generated name is literal: nothing may take a metacharacter in it for
    // a pattern.
    for (usize i = 0; i < name.size(); i++)
        f.mark.push(1);
    return out.push(move(f));
}

bool grow(Vec<String> &into, const String &prefix, Str name, bool slash)
{
    String s;
    if (!s.assign(prefix.str()) || !s.append(name))
        return false;
    if (slash && !s.push('/'))
        return false;
    return into.push(move(s));
}

// One field, already known to hold a live metacharacter. Appends every name it
// matched, or the field as it stands when it matched none.
Task<Result<void>> glob_one(const Field &f, Vec<Field> &out)
{
    Str text   = f.text.str();
    Bytes mask = mask_of(f);

    Walk *w = heap_new<Walk>();
    if (!w)
        co_return Err(Error::NoMemory);
    struct Free {
        ~Free() { heap_delete(p); }

        Walk *p;
    } guard{ w };

    usize i = 0;
    String base;
    if (unmarked_slash(f, 0)) {
        if (!base.push('/'))
            co_return Err(Error::NoMemory);
        i = 1;
    }
    if (!w->at.push(move(base)))
        co_return Err(Error::NoMemory);

    // A pattern ending in '/' names directories, as `echo */` does.
    bool trailing = text.size() > 1 && unmarked_slash(f, text.size() - 1);

    while (i < text.size()) {
        usize start = i;
        while (i < text.size() && !unmarked_slash(f, i))
            i++;
        usize len = i - start;
        while (i < text.size() && unmarked_slash(f, i))
            i++;
        if (len == 0)
            continue;

        Str comp  = text.substr(start, len);
        Bytes cm  = mask.subspan(start, len);
        bool dirs = i < text.size() || trailing; // a component to descend into
        w->next.clear();

        if (!glob_meta(comp, cm)) {
            // A literal component is taken on trust: the listing that follows
            // is what says whether it was there.
            for (const String &p : w->at)
                if (!grow(w->next, p, comp, dirs))
                    co_return Err(Error::NoMemory);
        } else {
            for (const String &p : w->at) {
                Result<Vec<DirEntry>> r = Err(Error::NoMemory);
                if (Task<Result<Vec<DirEntry>>> t = list_dir(dir_of(p)))
                    r = co_await t;
                if (r.is_err()) {
                    if (r.error() == Error::Cancelled)
                        co_return Err(Error::Cancelled);
                    continue; // not a directory, or gone: it matches nothing
                }
                w->ents = move(r.value());
                for (const DirEntry &e : w->ents) {
                    // A listing never resolves a link, so a component to
                    // descend into costs a stat when it is one.
                    if (dirs && e.kind == SYS_KIND_LINK) {
                        String full;
                        if (!full.assign(p.str()) || !full.append(e.name.str()))
                            co_return Err(Error::NoMemory);
                        Result<FileInfo> s = Err(Error::NotFound);
                        if (Task<Result<FileInfo>> t = stat_of(full.str()))
                            s = co_await t;
                        if (s.is_err() && s.error() == Error::Cancelled)
                            co_return Err(Error::Cancelled);
                        if (s.is_err() || s.value().kind != SYS_KIND_DIR)
                            continue;
                    } else if (dirs && e.kind != SYS_KIND_DIR) {
                        continue;
                    }
                    // A leading dot has to be asked for.
                    if (e.name.size() && e.name[0] == '.' && comp[0] != '.')
                        continue;
                    if (!glob_match(comp, cm, e.name.str()))
                        continue;
                    if (!grow(w->next, p, e.name.str(), dirs))
                        co_return Err(Error::NoMemory);
                }
            }
        }
        swap(w->at, w->next);
    }

    // Nothing matched, so the word stands as it was written — Bourne's rule.
    if (w->at.empty()) {
        Field copy;
        if (!copy.text.assign(text) || !copy.mark.reserve(f.mark.size()))
            co_return Err(Error::NoMemory);
        for (usize k = 0; k < f.mark.size(); k++)
            copy.mark.push(f.mark[k]);
        copy.quoted = f.quoted;
        if (!out.push(move(copy)))
            co_return Err(Error::NoMemory);
        co_return {};
    }

    for (const String &p : w->at)
        if (!emit(out, p.str()))
            co_return Err(Error::NoMemory);
    co_return {};
}

} // namespace

Task<Result<void>> glob_fields(Vec<Field> &in, Vec<Field> &out)
{
    for (Field &f : in) {
        if (!glob_meta(f.text.str(), mask_of(f))) {
            if (!out.push(move(f)))
                co_return Err(Error::NoMemory);
            continue;
        }
        Task<Result<void>> t = glob_one(f, out);
        Result<void> r       = t ? co_await t : Err(Error::NoMemory);
        if (r.is_err())
            co_return Err(r.error());
    }
    in.clear();
    co_return {};
}

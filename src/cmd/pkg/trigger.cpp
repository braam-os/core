#include "trigger.h"

#include "cmd/sh/match.h"
#include "dep.h"

namespace {

// The next `/`-separated component. A leading `/` yields an empty first one.
bool component(Str &rest, Str &out)
{
    if (rest.empty())
        return false;
    out = rest.split('/', rest);
    return true;
}

} // namespace

bool trigger_match(Str pattern, Str path)
{
    Str p = pattern, s = path, a, b;
    while (component(p, a)) {
        if (!component(s, b) || !glob_match(a, Span<const u8>(), b))
            return false;
    }
    return s.empty();
}

bool trigger_dirs(Str globs, bool fresh, Span<const TriggerDir> dirs, Vec<Str> &out, bool &fire)
{
    fire = false;

    for (const TriggerDir &d : dirs) {
        // apk: an unmodified directory is only a fresh package's business.
        if (!fresh && !d.modified)
            continue;

        Str rest = globs, glob;
        while (dep_next(rest, glob)) {
            bool only_changed = !glob.empty() && glob[0] == '+';
            if (only_changed)
                glob = glob.substr(1);
            if (glob.empty() || glob[0] != '/')
                continue;
            if (!trigger_match(glob, d.path))
                continue;

            // The first glob that matches settles this directory, argv or not.
            fire = true;
            if ((!only_changed || d.modified) && !out.push(d.path))
                return false;
            break;
        }
    }
    return true;
}

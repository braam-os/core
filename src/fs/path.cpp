#include "path.h"

namespace {

// Drops the last component of `out`, which always leaves at least "/".
void pop_component(String &out)
{
    usize n = out.size();
    while (n > 1 && out[n - 1] != '/')
        n--;
    if (n > 1)
        n--; // the separator itself, unless it is the root's
    out.truncate(n);
}

} // namespace

Result<void> path_resolve(Str cwd, Str p, String &out)
{
    out.clear();
    if (!out.push('/'))
        return Err(Error::NoMemory);

    // A relative path starts from the cwd, which is itself already normalised,
    // so it can be replayed through the same loop rather than copied.
    Str parts[2] = { p.starts_with("/") ? Str() : cwd, p };
    for (Str s : parts) {
        while (!s.empty()) {
            Str name = s.split('/', s);
            if (name.empty() || name == ".")
                continue;
            if (name == "..") {
                pop_component(out);
                continue;
            }
            if (out.size() > 1 && !out.push('/'))
                return Err(Error::NoMemory);
            if (!out.append(name))
                return Err(Error::NoMemory);
        }
    }
    return {};
}

Str path_dirname(Str p)
{
    usize i = p.size();
    while (i > 1 && p[i - 1] != '/')
        i--;
    if (i > 1)
        i--;
    return p.substr(0, i ? i : 1);
}

Str path_basename(Str p)
{
    usize i = p.size();
    while (i > 0 && p[i - 1] != '/')
        i--;
    Str name = p.substr(i);
    return name.empty() ? Str("/") : name;
}

Result<void> path_join(Str dir, Str name, String &out)
{
    out.clear();
    if (!out.append(dir))
        return Err(Error::NoMemory);
    if (!out.empty() && out[out.size() - 1] != '/' && !out.push('/'))
        return Err(Error::NoMemory);
    if (!out.append(name))
        return Err(Error::NoMemory);
    return {};
}

bool path_under(Str prefix, Str p)
{
    if (!p.starts_with(prefix))
        return false;
    return prefix.size() == 1 || p.size() == prefix.size() || p[prefix.size()] == '/';
}

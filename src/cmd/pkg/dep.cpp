#include "dep.h"

namespace {

constexpr bool is_op(char c)
{
    return c == '<' || c == '>' || c == '=' || c == '~';
}

constexpr bool is_sep(char c)
{
    return c == ' ' || c == '\n';
}

} // namespace

DepParse dep_parse(Str spec, Dep &out)
{
    Dep d;
    if (spec.starts_with("!")) {
        d.mask |= VER_CONFLICT;
        spec = spec.substr(1);
    }

    usize at = 0;
    while (at < spec.size() && !is_op(spec[at]))
        at++;
    d.name = spec.substr(0, at);
    if (d.name.empty())
        return DepParse::Malformed;

    if (at < spec.size()) {
        usize end = at;
        while (end < spec.size() && is_op(spec[end]))
            end++;
        d.version = spec.substr(end);
        if (d.version.empty())
            return DepParse::Malformed;
        d.mask   = (d.mask & VER_CONFLICT) | version_mask(spec.substr(at, end - at));
        d.broken = !version_valid(d.version);
    }

    out = d;
    return d.broken ? DepParse::Broken : DepParse::Ok;
}

bool dep_next(Str &rest, Str &spec)
{
    usize at = 0;
    while (at < rest.size() && is_sep(rest[at]))
        at++;
    rest = rest.substr(at);
    if (rest.empty())
        return false;

    usize end = 0;
    while (end < rest.size() && !is_sep(rest[end]))
        end++;
    spec = rest.substr(0, end);
    rest = rest.substr(end);
    return true;
}

bool dep_satisfied(const Dep &d, Str version)
{
    if (d.broken)
        return false;
    return version_match(version, d.mask, d.version);
}

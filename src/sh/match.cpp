#include "match.h"

namespace {

// Past the mask's end is unmarked, so an empty span means nothing was quoted.
bool marked(Span<const u8> mark, usize i)
{
    return i < mark.size() && mark[i] != 0;
}

bool live(Str pat, Span<const u8> mark, usize i, char c)
{
    return pat[i] == c && !marked(mark, i);
}

// A `[…]` group against one byte. `p` is the `[` and ends past the `]`; an
// unterminated group matches nothing.
bool bracket(Str pat, Span<const u8> mark, usize &p, char c)
{
    usize i  = p + 1;
    bool neg = false;
    if (i < pat.size() && live(pat, mark, i, '!')) {
        neg = true;
        i++;
    }

    bool hit    = false;
    bool closed = false;
    bool ranged = false; // the byte before could open a range
    char lo     = 0;

    for (; i < pat.size(); i++) {
        if (live(pat, mark, i, ']')) {
            closed = true;
            i++;
            break;
        }
        // A `-` is a range only between two bytes; last in the group it is itself.
        if (live(pat, mark, i, '-') && ranged && i + 1 < pat.size() &&
            !live(pat, mark, i + 1, ']')) {
            i++;
            if (u8(c) >= u8(lo) && u8(c) <= u8(pat[i]))
                hit = true;
            ranged = false;
            continue;
        }
        lo     = pat[i];
        ranged = true;
        if (c == lo)
            hit = true;
    }

    p = i;
    if (!closed)
        return false;
    return neg ? !hit : hit;
}

// One element — a byte, a `?`, or a `[…]` — against one byte of the name.
// `p` ends past the element when it matched.
bool element(Str pat, Span<const u8> mark, usize &p, char c)
{
    if (live(pat, mark, p, '?')) {
        p++;
        return true;
    }
    if (live(pat, mark, p, '['))
        return bracket(pat, mark, p, c);
    return pat[p++] == c;
}

} // namespace

// One saved backtrack point — the last `*` and the offset it was tried at.
bool glob_match(Str pattern, Span<const u8> mark, Str name)
{
    usize p    = 0;
    usize i    = 0;
    usize star = Str::npos;
    usize back = 0;

    while (i < name.size()) {
        usize next = p;
        if (p < pattern.size() && !live(pattern, mark, p, '*') &&
            element(pattern, mark, next, name[i])) {
            p = next;
            i++;
        } else if (p < pattern.size() && live(pattern, mark, p, '*')) {
            star = p++;
            back = i;
        } else if (star != Str::npos) {
            p = star + 1;
            i = ++back;
        } else {
            return false;
        }
    }

    // What is left of the pattern must be able to match nothing at all.
    while (p < pattern.size() && live(pattern, mark, p, '*'))
        p++;
    return p == pattern.size();
}

bool glob_meta(Str pattern, Span<const u8> mark)
{
    for (usize i = 0; i < pattern.size(); i++)
        if (live(pattern, mark, i, '*') || live(pattern, mark, i, '?') ||
            live(pattern, mark, i, '['))
            return true;
    return false;
}

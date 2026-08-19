// What a variable name is: a letter or `_`, then letters, digits and `_`.
#pragma once

#include "kernel/str.h"

inline bool is_name_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

inline bool is_name_char(char c)
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

inline bool is_name(Str w)
{
    if (w.empty() || !is_name_start(w[0]))
        return false;
    for (usize i = 1; i < w.size(); i++)
        if (!is_name_char(w[i]))
            return false;
    return true;
}

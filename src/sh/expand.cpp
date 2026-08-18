#include "expand.h"

namespace {

bool put(Field &f, char c, bool quoted)
{
    return f.text.push(c) && f.mark.push(quoted ? 1 : 0);
}

} // namespace

Result<void> expand_word(Str raw, Vec<Field> &out)
{
    Field f;

    for (usize i = 0; i < raw.size();) {
        char c = raw[i];

        if (c == '\'') {
            usize end = raw.find('\'', i + 1);
            if (end == Str::npos)
                return Err(Error::Invalid);
            for (usize j = i + 1; j < end; j++)
                if (!put(f, raw[j], true))
                    return Err(Error::NoMemory);
            i = end + 1;
            continue;
        }

        if (c == '"') {
            usize j = i + 1;
            for (; j < raw.size() && raw[j] != '"'; j++) {
                // Only a quote or a backslash is escapable in here; anything
                // else keeps its backslash.
                if (raw[j] == '\\' && j + 1 < raw.size() &&
                    (raw[j + 1] == '"' || raw[j + 1] == '\\'))
                    j++;
                if (!put(f, raw[j], true))
                    return Err(Error::NoMemory);
            }
            if (j >= raw.size())
                return Err(Error::Invalid);
            i = j + 1;
            continue;
        }

        if (c == '\\') {
            if (i + 1 >= raw.size())
                return Err(Error::Invalid);
            if (!put(f, raw[i + 1], true))
                return Err(Error::NoMemory);
            i += 2;
            continue;
        }

        if (!put(f, c, false))
            return Err(Error::NoMemory);
        i++;
    }

    if (!out.push(move(f)))
        return Err(Error::NoMemory);
    return {};
}

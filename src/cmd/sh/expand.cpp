#include "expand.h"

#include "name.h"
#include "tokenize.h"

namespace {

// `${x-${y-…}}` nests, and this recurses on the wasm stack.
constexpr u32 MAX_DEPTH = 8;

bool is_digit_char(char c)
{
    return c >= '0' && c <= '9';
}

// The parameters that are one punctuation character rather than a name.
bool is_special(char c)
{
    // `-` is `set`'s option letters, which is why it is not a name character.
    return c == '?' || c == '$' || c == '!' || c == '#' || c == '*' || c == '@' || c == '-';
}

// IFS, copied: `${x=y}` mid-walk may move the table that answered the lookup.
struct Ifs {
    char buf[16];
    usize n = 0;

    void load(const Vars &v)
    {
        Str s;
        if (!v.look(v.ctx, "IFS", s))
            s = " \t\n";
        n = s.size() < sizeof(buf) ? s.size() : sizeof(buf);
        for (usize i = 0; i < n; i++)
            buf[i] = s[i];
    }

    bool has(char c) const
    {
        for (usize i = 0; i < n; i++)
            if (buf[i] == c)
                return true;
        return false;
    }
};

struct Walk {
    Walk(const Vars &vars, Split sp, Str word, Vec<Field> &o, ExpandErr &e)
        : v(vars), split(sp), raw(word), out(o), err(e)
    {
        ifs.load(vars);
    }

    // Where a substitution begins in `raw`. Every recursion below is over a
    // substring of it, so the difference of the pointers is the offset and no
    // base has to be threaded through the walk.
    usize offset(Str s, usize i) const { return usize(s.data() + i - raw.data()); }

    const Vars &v;
    Split split;
    Str raw;
    Vec<Field> &out;
    ExpandErr &err;
    Ifs ifs;
    Field cur;

    Result<void> fail(Str m)
    {
        err.message = m;
        return Err(Error::Invalid);
    }

    bool put(char c, bool quoted) { return cur.text.push(c) && cur.mark.push(quoted ? 1 : 0); }

    // Ends the field here. A run of separators makes one break, and a field
    // that quoting brought into being survives even with nothing in it.
    bool brk()
    {
        if (cur.text.empty() && !cur.quoted)
            return true;
        if (!out.push(move(cur)))
            return false;
        cur = Field();
        return true;
    }

    // The last field. `Split::One` always has one; splitting drops an empty.
    bool finish()
    {
        if (split == Split::One)
            return out.push(move(cur));
        return brk();
    }

    // Bytes an expansion produced. Quoted ones are marked and never split; a
    // literal byte of the word never reaches here, so `IFS=:` leaves `a:b` one.
    bool put_value(Str s, bool quoted)
    {
        for (usize i = 0; i < s.size(); i++) {
            if (!quoted && split == Split::Fields && ifs.has(s[i])) {
                if (!brk())
                    return false;
                continue;
            }
            if (!put(s[i], quoted))
                return false;
        }
        return true;
    }

    Result<void> run(Str s, bool in_quotes, bool literal, u32 depth);
    Result<void> dollar(Str s, usize &i, bool in_quotes, u32 depth);
    Result<void> braced(Str body, bool in_quotes, u32 depth);
    Result<void> named(Str name, bool in_quotes);
    Result<void> positional(Str name, usize i, bool in_quotes);
    Result<void> unset(Str name);
    Result<void> stars(char which, bool in_quotes);
    Result<void> emit(Str name, bool in_quotes);
    Result<void> command(Str body, usize at, bool ticked, bool in_quotes);
    bool is_set(Str name);
};

// A nested expansion as text rather than fields: what `${x=y}` stores.
Result<void> to_string(Str w, const Vars &v, Str raw, ExpandErr &err, u32 depth, String &into)
{
    Vec<Field> tmp;
    Walk sub(v, Split::One, raw, tmp, err);
    TRY_VOID(sub.run(w, false, false, depth));
    if (!sub.finish())
        return Err(Error::NoMemory);
    into = move(tmp[0].text);
    return {};
}

// `set -u` is checked in these two rather than in emit(): a bare `$x` reaches
// them straight from dollar(), and `${x-y}` only ever arrives with an answer.
Result<void> Walk::unset(Str name)
{
    err.name    = name;
    err.message = "parameter not set";
    return Err(Error::Invalid);
}

Result<void> Walk::named(Str name, bool in_quotes)
{
    Str val;
    if (!v.look(v.ctx, name, val))
        return v.nounset ? unset(name) : Result<void>{};
    if (!put_value(val, in_quotes))
        return Err(Error::NoMemory);
    return {};
}

Result<void> Walk::positional(Str name, usize i, bool in_quotes)
{
    if (v.nounset && i && i > v.count(v.ctx))
        return unset(name);
    if (!put_value(v.at(v.ctx, i), in_quotes))
        return Err(Error::NoMemory);
    return {};
}

// `$*` and `$@`, which differ only inside quotes: `"$@"` breaks between the
// parameters and `"$*"` joins them with IFS's first byte. Unquoted they are the
// same thing, since both then split like any other expansion.
Result<void> Walk::stars(char which, bool in_quotes)
{
    bool each = !in_quotes || which == '@';
    usize n   = v.count(v.ctx);
    char join = ifs.n ? ifs.buf[0] : '\0';

    for (usize k = 1; k <= n; k++) {
        if (k > 1) {
            if (!each) {
                if (join && !put(join, in_quotes))
                    return Err(Error::NoMemory);
            } else if (split == Split::Fields) {
                if (!brk())
                    return Err(Error::NoMemory);
            } else if (!put(' ', in_quotes)) {
                // One word was asked for, so the break becomes a space.
                return Err(Error::NoMemory);
            }
        }
        if (!put_value(v.at(v.ctx, k), in_quotes))
            return Err(Error::NoMemory);
    }
    return {};
}

Result<void> Walk::emit(Str name, bool in_quotes)
{
    if (name.size() == 1 && (name[0] == '*' || name[0] == '@'))
        return stars(name[0], in_quotes);
    if (name.size() == 1 && is_digit_char(name[0]))
        return positional(name, usize(name[0] - '0'), in_quotes);
    return named(name, in_quotes);
}

bool Walk::is_set(Str name)
{
    if (name.size() == 1 && (name[0] == '*' || name[0] == '@'))
        return v.count(v.ctx) > 0;
    if (name.size() == 1 && is_digit_char(name[0])) {
        usize d = usize(name[0] - '0');
        return d == 0 || d <= v.count(v.ctx);
    }
    Str val;
    return v.look(v.ctx, name, val);
}

// `${name}` and the four operators. There are no colon forms: `${x-y}` asks
// whether x is *set*, empty or not.
Result<void> Walk::braced(Str body, bool in_quotes, u32 depth)
{
    if (body.empty())
        return fail("bad substitution");

    usize j = 0;
    if (is_name_start(body[0]))
        while (j < body.size() && is_name_char(body[j]))
            j++;
    else if (is_digit_char(body[0]) || is_special(body[0]))
        j = 1;
    else
        return fail("bad substitution");

    Str name = body.substr(0, j);
    if (j == body.size())
        return emit(name, in_quotes);

    char op = body[j];
    if (op != '-' && op != '=' && op != '?' && op != '+')
        return fail("bad substitution");

    Str word = body.substr(j + 1);
    bool set = is_set(name);

    if (op == '+') {
        if (!set)
            return {};
        return run(word, in_quotes, false, depth + 1);
    }
    if (op == '-')
        return set ? emit(name, in_quotes) : run(word, in_quotes, false, depth + 1);
    if (op == '?') {
        if (set)
            return emit(name, in_quotes);
        // Used as typed: the message is a view into the word.
        err.name    = name;
        err.message = word.empty() ? Str("parameter not set") : word;
        return Err(Error::Invalid);
    }

    if (set)
        return emit(name, in_quotes);
    if (!is_name_start(name[0]))
        return fail("cannot assign to this parameter");

    String val;
    TRY_VOID(to_string(word, v, raw, err, depth + 1, val));
    if (!v.set(v.ctx, name, val.str())) {
        err.name    = name;
        err.message = "cannot be set";
        return Err(Error::Invalid);
    }
    if (!put_value(val.str(), in_quotes))
        return Err(Error::NoMemory);
    return {};
}

// `$(…)` and the backtick, which differ only in how they were written. The
// output splits and is left unmarked when it is unquoted, so a `*` out of one
// globs exactly as one out of a variable does.
Result<void> Walk::command(Str body, usize at, bool ticked, bool in_quotes)
{
    Str got;
    if (!v.substitute || !v.substitute(v.ctx, at, body, got)) {
        err.at      = at;
        err.command = body;
        err.ticked  = ticked;
        return Err(Error::Again);
    }

    // Trailing newlines go, so `x=$(pwd)` puts none in x.
    while (!got.empty() && got[got.size() - 1] == '\n')
        got = got.substr(0, got.size() - 1);

    if (!put_value(got, in_quotes))
        return Err(Error::NoMemory);
    return {};
}

Result<void> Walk::dollar(Str s, usize &i, bool in_quotes, u32 depth)
{
    // A `$` with nothing a parameter can be made of is an ordinary byte.
    char c = i + 1 < s.size() ? s[i + 1] : '\0';

    if (c == '{') {
        usize end = brace_end(s, i);
        if (end == Str::npos)
            return fail("unterminated ${");
        Str body = s.substr(i + 2, end - (i + 2));
        i        = end + 1;
        return braced(body, in_quotes, depth);
    }
    if (c == '(') {
        usize end = paren_end(s, i);
        if (end == Str::npos)
            return fail("unterminated $(");
        Str body = s.substr(i + 2, end - (i + 2));
        usize at = offset(s, i);
        i        = end + 1;
        return command(body, at, false, in_quotes);
    }
    if (is_name_start(c)) {
        usize j = i + 1;
        while (j < s.size() && is_name_char(s[j]))
            j++;
        Str name = s.substr(i + 1, j - (i + 1));
        i        = j;
        return named(name, in_quotes);
    }
    if (is_digit_char(c)) {
        Str name = s.substr(i + 1, 1);
        i += 2;
        return positional(name, usize(c - '0'), in_quotes);
    }
    if (c == '*' || c == '@') {
        i += 2;
        return stars(c, in_quotes);
    }
    if (is_special(c)) {
        Str name = s.substr(i + 1, 1);
        i += 2;
        return named(name, in_quotes);
    }

    i++;
    if (!put('$', in_quotes))
        return Err(Error::NoMemory);
    return {};
}

// `literal` says the bytes are the word's own, which never split: the lexer
// already ended the word. A `${x-…}` body is expansion output, so `${x-a b}`
// is two fields.
Result<void> Walk::run(Str s, bool in_quotes, bool literal, u32 depth)
{
    if (depth > MAX_DEPTH)
        return fail("expansion nested too deep");

    for (usize i = 0; i < s.size();) {
        char c = s[i];

        if (!in_quotes && c == '\'') {
            usize end = s.find('\'', i + 1);
            if (end == Str::npos)
                return fail("unterminated quote");
            cur.quoted = true;
            for (usize k = i + 1; k < end; k++)
                if (!put(s[k], true))
                    return Err(Error::NoMemory);
            i = end + 1;
            continue;
        }

        if (!in_quotes && c == '"') {
            usize j = i + 1;
            for (; j < s.size() && s[j] != '"'; j++) {
                // A `$(…)` in here quotes independently. Identical to the
                // lexer's scan, which is what ends the word.
                if (s[j] == '$' && j + 1 < s.size() && s[j + 1] == '(') {
                    usize end = paren_end(s, j);
                    if (end == Str::npos)
                        return fail("unterminated $(");
                    j = end;
                    continue;
                }
                // Only a quote, a backslash or a dollar is escapable in here.
                if (s[j] == '\\' && j + 1 < s.size() &&
                    (s[j + 1] == '"' || s[j + 1] == '\\' || s[j + 1] == '$'))
                    j++;
            }
            if (j >= s.size())
                return fail("unterminated quote");

            Str body = s.substr(i + 1, j - (i + 1));
            // `"$@"` with no parameters is the one quoted run leaving no
            // field at all, where `""` leaves an empty one.
            if (!(v.count(v.ctx) == 0 && (body == "$@" || body == "${@}")))
                cur.quoted = true;
            TRY_VOID(run(body, true, literal, depth));
            i = j + 1;
            continue;
        }

        if (c == '\\') {
            if (!in_quotes) {
                if (i + 1 >= s.size())
                    return fail("unterminated quote");
                if (!put(s[i + 1], true))
                    return Err(Error::NoMemory);
                i += 2;
                continue;
            }
            if (i + 1 < s.size() && (s[i + 1] == '"' || s[i + 1] == '\\' || s[i + 1] == '$')) {
                if (!put(s[i + 1], true))
                    return Err(Error::NoMemory);
                i += 2;
                continue;
            }
        }

        // Not guarded by `!in_quotes`, unlike the quote arms: a backtick works
        // inside `"…"`, which is most of what it is still used for.
        if (c == '`') {
            usize end = tick_end(s, i);
            if (end == Str::npos)
                return fail("unterminated `");
            Str body = s.substr(i + 1, end - (i + 1));
            usize at = offset(s, i);
            i        = end + 1;
            TRY_VOID(command(body, at, true, in_quotes));
            continue;
        }

        if (c == '$') {
            TRY_VOID(dollar(s, i, in_quotes, depth));
            continue;
        }

        if (literal) {
            if (!put(c, in_quotes))
                return Err(Error::NoMemory);
        } else if (!put_value(s.substr(i, 1), in_quotes))
            return Err(Error::NoMemory);
        i++;
    }
    return {};
}

} // namespace

Result<void> expand_word(Str raw, const Vars &v, Split split, Vec<Field> &out, ExpandErr &err)
{
    err = ExpandErr();

    Walk w(v, split, raw, out, err);
    TRY_VOID(w.run(raw, false, true, 0));
    if (!w.finish())
        return Err(Error::NoMemory);
    return {};
}

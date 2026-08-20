#include "version.h"

namespace {

// The order is the semantics: token_next bounds against it, and so does the
// tail of compare_fuzzy.
enum Token {
    TOKEN_INITIAL_DIGIT,
    TOKEN_DIGIT,
    TOKEN_LETTER,
    TOKEN_SUFFIX,
    TOKEN_SUFFIX_NO,
    TOKEN_COMMIT_HASH,
    TOKEN_REVISION_NO,
    TOKEN_END,
    TOKEN_INVALID,
};

// NONE is the pivot.
enum Suffix {
    SUFFIX_INVALID,
    SUFFIX_ALPHA,
    SUFFIX_BETA,
    SUFFIX_PRE,
    SUFFIX_RC,
    SUFFIX_NONE,
    SUFFIX_CVS,
    SUFFIX_SVN,
    SUFFIX_GIT,
    SUFFIX_HG,
    SUFFIX_P,
};

constexpr Str SUFFIX_NAME[] = {
    "", "alpha", "beta", "pre", "rc", "", "cvs", "svn", "git", "hg", "p"
};

u32 suffix_value(Str suf)
{
    if (suf.empty())
        return SUFFIX_INVALID;
    u32 val;
    switch (suf[0]) {
    case 'a':
        val = SUFFIX_ALPHA;
        break;
    case 'b':
        val = SUFFIX_BETA;
        break;
    case 'c':
        val = SUFFIX_CVS;
        break;
    case 'g':
        val = SUFFIX_GIT;
        break;
    case 'h':
        val = SUFFIX_HG;
        break;
    case 'p':
        val = suf.size() > 1 ? SUFFIX_PRE : SUFFIX_P;
        break;
    case 'r':
        val = SUFFIX_RC;
        break;
    case 's':
        val = SUFFIX_SVN;
        break;
    default:
        return SUFFIX_INVALID;
    }
    return suf == SUFFIX_NAME[val] ? val : SUFFIX_INVALID;
}

// apk's blob_sort: shared prefix, then length. Not blob_compare. Byte by byte,
// since there is no memcmp to call.
int str_sort(Str a, Str b)
{
    usize n = a.size() < b.size() ? a.size() : b.size();
    for (usize i = 0; i < n; i++)
        if (a[i] != b[i])
            return u8(a[i]) < u8(b[i]) ? -1 : 1;
    return int(a.size()) - int(b.size());
}

constexpr bool is_lower(char c)
{
    return c >= 'a' && c <= 'z';
}

constexpr bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// The accepted run, taken off the front of `rest`.
Str take_while(Str &rest, bool (*accept)(char))
{
    usize n = 0;
    while (n < rest.size() && accept(rest[n]))
        n++;
    Str out = rest.substr(0, n);
    rest    = rest.substr(n);
    return out;
}

struct TokenState {
    u32 token  = TOKEN_INVALID;
    u32 suffix = SUFFIX_INVALID;
    u64 number = 0;
    Str value;
};

void token_parse_digits(TokenState &t, Str &rest)
{
    Str digits = take_while(rest, [](char c) { return c >= '0' && c <= '9'; });
    u64 v      = 0;
    for (usize i = 0; i < digits.size(); i++)
        v = v * 10 + u64(digits[i] - '0');
    t.number = v;
    t.value  = digits;
    if (digits.empty())
        t.token = TOKEN_INVALID;
}

void token_first(TokenState &t, Str &rest)
{
    t.token = TOKEN_INITIAL_DIGIT;
    token_parse_digits(t, rest);
}

void token_next(TokenState &t, Str &rest)
{
    if (rest.empty()) {
        t.token = TOKEN_END;
        return;
    }
    char c = rest[0];
    if (c >= 'a' && c <= 'z') {
        if (t.token > TOKEN_DIGIT) {
            t.token = TOKEN_INVALID;
            return;
        }
        t.value = rest.substr(0, 1);
        t.token = TOKEN_LETTER;
        rest    = rest.substr(1);
        return;
    }
    if (c == '.') {
        if (t.token > TOKEN_DIGIT) {
            t.token = TOKEN_INVALID;
            return;
        }
        rest = rest.substr(1);
        c    = '0'; // fall through to the digit run
    }
    if (c >= '0' && c <= '9') {
        if (t.token == TOKEN_INITIAL_DIGIT || t.token == TOKEN_DIGIT)
            t.token = TOKEN_DIGIT;
        else if (t.token == TOKEN_SUFFIX)
            t.token = TOKEN_SUFFIX_NO;
        else {
            t.token = TOKEN_INVALID;
            return;
        }
        token_parse_digits(t, rest);
        return;
    }
    if (c == '_') {
        if (t.token > TOKEN_SUFFIX_NO) {
            t.token = TOKEN_INVALID;
            return;
        }
        rest     = rest.substr(1);
        t.value  = take_while(rest, is_lower);
        t.suffix = suffix_value(t.value);
        t.token  = t.suffix == SUFFIX_INVALID ? u32(TOKEN_INVALID) : u32(TOKEN_SUFFIX);
        return;
    }
    if (c == '~') {
        if (t.token >= TOKEN_COMMIT_HASH) {
            t.token = TOKEN_INVALID;
            return;
        }
        rest    = rest.substr(1);
        t.value = take_while(rest, is_hex);
        t.token = t.value.empty() ? u32(TOKEN_INVALID) : u32(TOKEN_COMMIT_HASH);
        return;
    }
    if (c == '-') {
        if (t.token >= TOKEN_REVISION_NO || !rest.starts_with("-r")) {
            t.token = TOKEN_INVALID;
            return;
        }
        rest    = rest.substr(2);
        t.token = TOKEN_REVISION_NO;
        token_parse_digits(t, rest);
        return;
    }
    t.token = TOKEN_INVALID;
}

u32 token_cmp(const TokenState &ta, const TokenState &tb)
{
    u64 a, b;
    switch (ta.token) {
    case TOKEN_DIGIT:
        // A leading zero on either side sorts as a string.
        if (ta.value[0] == '0' || tb.value[0] == '0')
            break;
        [[fallthrough]];
    case TOKEN_INITIAL_DIGIT:
    case TOKEN_SUFFIX_NO:
    case TOKEN_REVISION_NO:
        a = ta.number;
        b = tb.number;
        return a < b ? VER_LESS : a > b ? VER_GREATER : VER_EQUAL;
    case TOKEN_LETTER:
        a = u64(u8(ta.value[0]));
        b = u64(u8(tb.value[0]));
        return a < b ? VER_LESS : a > b ? VER_GREATER : VER_EQUAL;
    case TOKEN_SUFFIX:
        a = ta.suffix;
        b = tb.suffix;
        return a < b ? VER_LESS : a > b ? VER_GREATER : VER_EQUAL;
    default:
        break;
    }
    int r = str_sort(ta.value, tb.value);
    return r < 0 ? VER_LESS : r > 0 ? VER_GREATER : VER_EQUAL;
}

u32 compare_fuzzy(Str a, Str b, bool fuzzy)
{
    TokenState ta, tb;
    for (token_first(ta, a), token_first(tb, b); ta.token == tb.token && ta.token < TOKEN_END;
         token_next(ta, a), token_next(tb, b)) {
        u32 r = token_cmp(ta, tb);
        if (r != VER_EQUAL)
            return r;
    }

    // Both ended, both invalid at the same place, or the fuzzy right side ran out.
    if (ta.token == tb.token)
        return VER_EQUAL;
    if (tb.token == TOKEN_END && fuzzy)
        return VER_EQUAL;

    // The side with more is greater, unless what it has next is a pre-release.
    if (ta.token == TOKEN_SUFFIX && ta.suffix < SUFFIX_NONE)
        return VER_LESS;
    if (tb.token == TOKEN_SUFFIX && tb.suffix < SUFFIX_NONE)
        return VER_GREATER;
    if (ta.token > tb.token)
        return VER_LESS;
    if (tb.token > ta.token)
        return VER_GREATER;
    return VER_EQUAL;
}

} // namespace

bool version_valid(Str v)
{
    TokenState t;
    for (token_first(t, v); t.token < TOKEN_END; token_next(t, v))
        ;
    return t.token == TOKEN_END;
}

u32 version_compare(Str a, Str b)
{
    return compare_fuzzy(a, b, false);
}

u32 version_mask(Str op)
{
    u32 r = 0;
    for (usize i = 0; i < op.size(); i++) {
        switch (op[i]) {
        case '<':
            r |= VER_LESS;
            break;
        case '>':
            r |= VER_GREATER;
            break;
        case '=':
            r |= VER_EQUAL;
            break;
        case '~':
            r |= VER_FUZZY | VER_EQUAL;
            break;
        default:
            return 0;
        }
    }
    return r;
}

bool version_match(Str a, u32 mask, Str b)
{
    if ((mask & VER_ANY) == VER_ANY)
        return true;
    return (compare_fuzzy(a, b, (mask & VER_FUZZY) != 0) & mask) != 0;
}

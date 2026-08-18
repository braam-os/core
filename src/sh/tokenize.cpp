#include "tokenize.h"

#include "kernel/text.h"

namespace {

bool is_operator(char c)
{
    return c == '|' || c == '&' || c == '<' || c == '>' || c == ';';
}

// Everything is_space takes but a newline, which is a token of its own now
// that one text may be several lines.
bool is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

} // namespace

usize brace_end(Str s, usize at)
{
    if (at + 1 >= s.size() || s[at] != '$' || s[at + 1] != '{')
        return Str::npos;

    usize depth = 0;
    for (usize i = at + 1; i < s.size(); i++) {
        char c = s[i];

        if (c == '\\') {
            i++;
            continue;
        }
        if (c == '\'') {
            usize end = s.find('\'', i + 1);
            if (end == Str::npos)
                return Str::npos;
            i = end;
            continue;
        }
        if (c == '"') {
            usize j = i + 1;
            for (; j < s.size() && s[j] != '"'; j++)
                if (s[j] == '\\' && j + 1 < s.size())
                    j++;
            if (j >= s.size())
                return Str::npos;
            i = j;
            continue;
        }

        if (c == '{')
            depth++;
        else if (c == '}' && --depth == 0)
            return i;
    }
    return Str::npos;
}

Result<Tok> Lexer::next(Str &word)
{
    word = Str();

    for (;;) {
        while (i_ < s_.size() && is_blank(s_[i_]))
            i_++;
        // A comment only where a token could start, so `a#b` is one word.
        if (i_ >= s_.size() || s_[i_] != '#')
            break;
        while (i_ < s_.size() && s_[i_] != '\n')
            i_++;
    }
    begin_ = i_;
    if (i_ >= s_.size())
        return Tok::End;

    char c = s_[i_];
    if (c == '\n') {
        i_++;
        return Tok::Newline;
    }
    if (c == ';') {
        i_++;
        return Tok::Semi;
    }
    if (c == '|') {
        i_++;
        if (i_ < s_.size() && s_[i_] == '|') {
            i_++;
            return Tok::OrIf;
        }
        return Tok::Pipe;
    }
    if (c == '&') {
        i_++;
        if (i_ < s_.size() && s_[i_] == '&') {
            i_++;
            return Tok::AndIf;
        }
        return Tok::Amp;
    }
    if (c == '<') {
        i_++;
        return Tok::Less;
    }
    if (c == '>') {
        i_++;
        if (i_ < s_.size() && s_[i_] == '>') {
            i_++;
            return Tok::DGreat;
        }
        return Tok::Great;
    }
    // A file descriptor binds only when it is the whole prefix of the token:
    // `2>x` redirects stderr, `a2>x` is the word `a2` and then `>`.
    if (c == '2' && i_ + 1 < s_.size() && s_[i_ + 1] == '>') {
        i_ += 2;
        if (i_ < s_.size() && s_[i_] == '>') {
            i_++;
            return Tok::ErrDGreat;
        }
        return Tok::ErrGreat;
    }

    // A word runs to the next *unquoted* separator, so the quotes are walked
    // here to find where it ends — and left in it, for expand.h to take off.
    usize start = i_;
    for (; i_ < s_.size(); i_++) {
        c = s_[i_];
        if (is_space(c) || is_operator(c))
            break;

        // A `${…}` is one piece of the word however it reads inside, so the
        // body is walked rather than scanned for separators.
        if (c == '$' && i_ + 1 < s_.size() && s_[i_ + 1] == '{') {
            usize end = brace_end(s_, i_);
            if (end == Str::npos)
                return Err(Error::Invalid);
            i_ = end;
            continue;
        }

        if (c == '\'') {
            usize end = s_.find('\'', i_ + 1);
            if (end == Str::npos)
                return Err(Error::Invalid);
            i_ = end;
            continue;
        }

        if (c == '"') {
            usize j = i_ + 1;
            for (; j < s_.size() && s_[j] != '"'; j++)
                // Only a quote, a backslash or a dollar is escapable in here.
                if (s_[j] == '\\' && j + 1 < s_.size() &&
                    (s_[j + 1] == '"' || s_[j + 1] == '\\' || s_[j + 1] == '$'))
                    j++;
            if (j >= s_.size())
                return Err(Error::Invalid);
            i_ = j;
            continue;
        }

        if (c == '\\') {
            if (i_ + 1 >= s_.size())
                return Err(Error::Invalid);
            i_++;
        }
    }

    word = s_.substr(start, i_ - start);
    return Tok::Word;
}

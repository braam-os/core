#include "tokenize.h"

#include "kernel/text.h"

namespace {

bool is_operator(char c)
{
    return c == '|' || c == '&' || c == '<' || c == '>';
}

} // namespace

Result<Tok> Lexer::next(Str &word)
{
    word = Str();

    while (i_ < s_.size() && is_space(s_[i_]))
        i_++;
    if (i_ >= s_.size())
        return Tok::End;

    char c = s_[i_];
    if (c == '|') {
        i_++;
        return Tok::Pipe;
    }
    if (c == '&') {
        i_++;
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
                // Only a quote or a backslash is escapable in here.
                if (s_[j] == '\\' && j + 1 < s_.size() && (s_[j + 1] == '"' || s_[j + 1] == '\\'))
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

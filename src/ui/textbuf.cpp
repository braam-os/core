#include "textbuf.h"

#include "kernel/text.h"
#include "kernel/traits.h"

namespace {

// A continuation byte belongs to the codepoint before it.
bool is_cont(char c)
{
    return (u8(c) & 0xc0) == 0x80;
}

} // namespace

Result<void> TextBuf::load(Str utf8)
{
    lines_.clear();
    usize at = 0;
    for (;;) {
        usize nl = utf8.find('\n', at);
        Str one  = nl == Str::npos ? utf8.substr(at) : utf8.substr(at, nl - at);

        String s;
        if (!s.assign(one) || !lines_.push(move(s)))
            return Err(Error::NoMemory);
        if (nl == Str::npos)
            break;
        at = nl + 1;
        if (at == utf8.size())
            break; // a trailing newline ends the last line, it does not add one
    }
    if (lines_.empty()) {
        String s;
        if (!lines_.push(move(s)))
            return Err(Error::NoMemory);
    }
    modified_ = false;
    return {};
}

Result<void> TextBuf::add(Str line)
{
    String s;
    if (!s.assign(line) || !lines_.push(move(s)))
        return Err(Error::NoMemory);
    return {};
}

Result<void> TextBuf::serialize(String &out) const
{
    out.clear();
    for (usize i = 0; i < lines_.size(); i++)
        if (!out.append(lines_[i].str()) || !out.push('\n'))
            return Err(Error::NoMemory);
    return {};
}

Str TextBuf::line(usize i) const
{
    return i < lines_.size() ? lines_[i].str() : Str();
}

Result<void> TextBuf::ensure(usize row)
{
    while (lines_.size() <= row) {
        String s;
        if (!lines_.push(move(s)))
            return Err(Error::NoMemory);
    }
    return {};
}

Result<void> TextBuf::insert(usize row, usize at, Str utf8)
{
    TRY_VOID(ensure(row));
    if (!lines_[row].insert(at, utf8))
        return Err(Error::NoMemory);
    modified_ = true;
    return {};
}

usize TextBuf::erase(usize row, usize at)
{
    if (row >= lines_.size() || at >= lines_[row].size())
        return 0;
    usize end = next(row, at);
    lines_[row].erase(at, end - at);
    modified_ = true;
    return end - at;
}

Result<void> TextBuf::split(usize row, usize at)
{
    TRY_VOID(ensure(row));
    String tail;
    Str src = lines_[row].str();
    if (at > src.size())
        at = src.size();
    if (!tail.assign(src.substr(at)))
        return Err(Error::NoMemory);
    if (!lines_.insert(row + 1, move(tail)))
        return Err(Error::NoMemory);
    lines_[row].truncate(at);
    modified_ = true;
    return {};
}

Result<usize> TextBuf::join(usize row)
{
    if (row + 1 >= lines_.size())
        return Err(Error::Invalid);
    usize at = lines_[row].size();
    if (!lines_[row].append(lines_[row + 1].str()))
        return Err(Error::NoMemory);
    lines_.erase(row + 1);
    modified_ = true;
    return at;
}

usize TextBuf::prev(usize row, usize at) const
{
    Str s = line(row);
    if (at > s.size())
        at = s.size();
    while (at > 0) {
        at--;
        if (!is_cont(s[at]))
            break;
    }
    return at;
}

usize TextBuf::next(usize row, usize at) const
{
    Str s = line(row);
    if (at >= s.size())
        return s.size();
    at++;
    while (at < s.size() && is_cont(s[at]))
        at++;
    return at;
}

usize TextBuf::column(usize row, usize at) const
{
    Str s   = line(row);
    usize n = 0;
    for (usize i = 0; i < at && i < s.size(); i++)
        if (!is_cont(s[i]))
            n++;
    return n;
}

usize TextBuf::offset(usize row, usize col) const
{
    Str s   = line(row);
    usize i = 0;
    while (i < s.size() && col) {
        i = next(row, i);
        col--;
    }
    return i;
}

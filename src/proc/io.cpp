#include "io.h"

Task<Result<void>> write_all(u32 fd, Str s)
{
    while (!s.empty()) {
        Result<SysReply> r = co_await sys_call(Sys::Write, fd, s);
        if (r.is_err())
            co_return Err(r.error());
        usize n = usize(r.value().status);
        if (n == 0)
            co_return Err(Error::Io);
        s = s.substr(n);
    }
    co_return {};
}

Task<Result<String>> read_chunk(u32 fd)
{
    Result<SysReply> r = co_await sys_call(Sys::Read, fd);
    if (r.is_err())
        co_return Err(r.error());
    if (r.value().data.empty())
        co_return Err(Error::Closed); // end of input

    String s;
    if (!s.assign(r.value().data))
        co_return Err(Error::NoMemory);
    co_return move(s);
}

Task<Result<i32>> open_read(Str path)
{
    String req;
    if (!req.reserve(4 + path.size()))
        co_return Err(Error::NoMemory);
    req.append(Str("\0\0\0\0", 4));
    req.append(path);
    sys_put_u32(reinterpret_cast<u8 *>(req.data()), SYS_O_READ);

    Result<SysReply> r = co_await sys_call(Sys::Open, 0, req.str());
    if (r.is_err())
        co_return Err(r.error());
    co_return r.value().status;
}

Task<void> close_fd(u32 fd)
{
    co_await sys_call(Sys::Close, fd);
}

Task<void> errln(Str who, Str what, Error why)
{
    String line;
    line.append(who);
    line.append(": ");
    if (!what.empty()) {
        line.append(what);
        line.append(": ");
    }
    line.append(error_name(why));
    line.push('\n');
    co_await write_all(SYS_STDERR, line.str());
}

Task<i32> Input::open_all(Str who)
{
    if (paths_.size() == 0)
        co_return 0;
    own_ = true;

    for (usize i = 0; i < paths_.size(); i++) {
        Task<Result<i32>> t = open_read(paths_[i]);
        if (!t)
            co_return 1;
        Result<i32> r = co_await t;
        if (r.is_err()) {
            if (Task<void> e = errln(who, paths_[i], r.error()))
                co_await e;
            co_return r.error() == Error::Cancelled ? 130 : 1;
        }
        if (!fds_.push(r.value())) {
            if (Task<void> c = close_fd(u32(r.value())))
                co_await c;
            co_return 1;
        }
    }
    co_return 0;
}

// End of one file is not end of input: it is the start of the next.
Task<Result<String>> Input::read()
{
    if (!own_) {
        Task<Result<String>> t = read_chunk(fd_);
        if (!t)
            co_return Err(Error::NoMemory);
        co_return co_await t;
    }

    while (at_ < fds_.size()) {
        Task<Result<String>> t = read_chunk(u32(fds_[at_]));
        if (!t)
            co_return Err(Error::NoMemory);
        Result<String> r = co_await t;
        if (r.is_ok() || r.error() != Error::Closed)
            co_return move(r);
        if (Task<void> c = close_fd(u32(fds_[at_])))
            co_await c;
        at_++;
    }
    co_return Err(Error::Closed);
}

Task<Result<bool>> LineReader::next(String &out)
{
    out.clear();
    for (;;) {
        for (usize i = pos_; i < buf_.size(); i++) {
            if (buf_[i] != '\n')
                continue;
            if (!out.append(Str(buf_.data() + pos_, i - pos_)))
                co_return Err(Error::NoMemory);
            pos_ = i + 1;
            if (pos_ == buf_.size()) {
                buf_.clear();
                pos_ = 0;
            }
            co_return true;
        }

        if (eof_) {
            if (pos_ == buf_.size())
                co_return false;
            if (!out.append(Str(buf_.data() + pos_, buf_.size() - pos_)))
                co_return Err(Error::NoMemory);
            buf_.clear();
            pos_ = 0;
            co_return true;
        }

        Task<Result<String>> t = in_.read();
        if (!t)
            co_return Err(Error::NoMemory);
        Result<String> r = co_await t;
        if (r.is_err()) {
            if (r.error() != Error::Closed)
                co_return Err(r.error());
            eof_ = true;
            continue;
        }

        // The unread tail slides down before the buffer takes more, so a
        // long-running reader does not grow it without bound.
        if (pos_ > 0) {
            usize rest = buf_.size() - pos_;
            __builtin_memmove(buf_.data(), buf_.data() + pos_, rest);
            buf_.truncate(rest);
            pos_ = 0;
        }
        if (!buf_.append(r.value().str()))
            co_return Err(Error::NoMemory);
    }
}

#include "io.h"

namespace {

Result<usize> pipe_write(void *ctx, Str s)
{
    Pipe &p = *static_cast<Pipe *>(ctx);
    if (p.closed() || p.hung_up())
        return Err(Error::Closed);
    if (s.empty())
        return usize(0);
    if (p.full())
        return Err(Error::Again);

    String chunk;
    if (!chunk.assign(s))
        return Err(Error::NoMemory);
    if (!p.try_send(move(chunk)))
        return Err(Error::Again);
    return s.size();
}

void pipe_park_writer(void *ctx, u32 token, bool on)
{
    static_cast<Pipe *>(ctx)->park_sender(token, on);
}

Result<String> pipe_read(void *ctx)
{
    Pipe &p          = *static_cast<Pipe *>(ctx);
    Option<String> v = p.try_recv();
    if (v.has_value())
        return move(v.value());
    if (p.closed())
        return Err(Error::Closed);
    return Err(Error::Again);
}

void pipe_park_reader(void *ctx, u32 token, bool on)
{
    static_cast<Pipe *>(ctx)->park_receiver(token, on);
}

Result<String> read_nothing(void *)
{
    return Err(Error::Closed);
}

} // namespace

Stream pipe_sink(Pipe &p)
{
    return Stream{ pipe_write, pipe_park_writer, &p };
}

Source pipe_source(Pipe &p)
{
    return Source{ pipe_read, pipe_park_reader, &p };
}

Source null_source()
{
    return Source{ read_nothing, nullptr, nullptr };
}

Task<Result<void>> write_all(Stream out, Str s)
{
    for (;;) {
        Result<usize> r = co_await out.write(s);
        if (r.is_ok()) {
            if (r.value() >= s.size())
                co_return {};
            s = s.substr(r.value());
            continue;
        }
        if (r.error() != Error::Again)
            co_return Err(r.error());
    }
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

        Result<String> r = co_await in_.read();
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

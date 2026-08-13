#include "io.h"

#include "kernel/alloc.h"

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

Result<usize> file_write(void *ctx, Str s)
{
    FileIo &f = *static_cast<FileIo *>(ctx);
    if (f.fd < 0)
        return Err(Error::Closed);
    if (s.empty())
        return usize(0);

    Result<usize> r = vfs_write(f.fd, f.off, reinterpret_cast<const u8 *>(s.data()), s.size());
    if (r.is_ok())
        f.off += r.value();
    return r;
}

Result<String> file_read(void *ctx)
{
    FileIo &f = *static_cast<FileIo *>(ctx);
    if (f.fd < 0)
        return Err(Error::Closed);

    // One block per read: FS_BLOCK is the allocator's top size class, so the
    // chunk this hands to a pipe costs one block and not a whole span
    // (Concept.md §8.2). Reading is synchronous, so the staging buffer can sit
    // on the stack rather than in a frame.
    u8 block[FS_BLOCK];
    Result<usize> r = vfs_read(f.fd, f.off, block, sizeof(block));
    if (r.is_err())
        return Err(r.error());
    if (r.value() == 0)
        return Err(Error::Closed); // end of file, which is end of input
    f.off += r.value();

    String chunk;
    if (!chunk.assign(Str(reinterpret_cast<const char *>(block), r.value())))
        return Err(Error::NoMemory);
    return move(chunk);
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

Stream file_sink(FileIo &f)
{
    return Stream{ file_write, nullptr, &f };
}

Source file_source(FileIo &f)
{
    return Source{ file_read, nullptr, &f };
}

Task<Result<void>> file_open_read(Str path, FileIo &out)
{
    Task<Result<i32>> t = vfs_open(path, O_READ);
    if (!t)
        co_return Err(Error::NoMemory);

    out.reset();
    out.fd  = CO_TRY(co_await t);
    out.off = 0;
    co_return {};
}

Inputs::~Inputs()
{
    for (FileIo *f : files_)
        heap_delete(f);
}

Task<Result<void>> Inputs::open(Args paths, Str &failed)
{
    for (usize i = 0; i < paths.size(); i++) {
        failed    = paths[i];
        FileIo *f = heap_new<FileIo>();
        if (!f || !files_.push(f)) {
            heap_delete(f);
            co_return Err(Error::NoMemory);
        }
        Task<Result<void>> t = file_open_read(paths[i], *f);
        if (!t)
            co_return Err(Error::NoMemory);
        CO_TRY_VOID(co_await t);
    }
    failed = Str();
    co_return {};
}

// End of one file is not end of input: it is the start of the next.
Result<String> Inputs::read_next(void *ctx)
{
    Inputs &in = *static_cast<Inputs *>(ctx);
    while (in.at_ < in.files_.size()) {
        Result<String> r = file_read(in.files_[in.at_]);
        if (r.is_ok() || r.error() != Error::Closed)
            return r;
        in.at_++;
    }
    return Err(Error::Closed);
}

Source Inputs::source()
{
    return Source{ read_next, nullptr, this };
}

Task<i32> open_inputs(Inputs &in, Args paths, Str who, Stdio io)
{
    if (paths.size() == 0)
        co_return 0;

    Str failed;
    Result<void> r = Err(Error::NoMemory);
    if (Task<Result<void>> t = in.open(paths, failed))
        r = co_await t;
    if (r.is_ok())
        co_return 0;

    co_await io.err.write(who);
    co_await io.err.write(": ");
    co_await io.err.write(failed);
    co_await io.err.write(": ");
    co_await io.err.write(error_name(r.error()));
    co_await io.err.write("\n");
    co_return r.error() == Error::Cancelled ? 130 : 1;
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

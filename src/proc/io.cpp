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

Task<Result<i32>> open_at(Str path, u32 flags)
{
    Result<SysReply> r = co_await sys_call(Sys::Open, flags, path);
    if (r.is_err())
        co_return Err(r.error());
    co_return r.value().status;
}

Task<Result<i32>> open_read(Str path)
{
    co_return co_await open_at(path, SYS_O_READ);
}

Task<void> close_fd(u32 fd)
{
    co_await sys_call(Sys::Close, fd);
}

Task<Result<String>> read_file(Str path)
{
    Task<Result<i32>> o = open_read(path);
    if (!o)
        co_return Err(Error::NoMemory);
    Result<i32> fd = co_await o;
    if (fd.is_err())
        co_return Err(fd.error());

    String out;
    Result<void> bad = {};
    for (;;) {
        Task<Result<String>> t = read_chunk(u32(fd.value()));
        if (!t) {
            bad = Err(Error::NoMemory);
            break;
        }
        Result<String> r = co_await t;
        if (r.is_err()) {
            if (r.error() != Error::Closed)
                bad = Err(r.error());
            break;
        }
        if (!out.append(r.value().str())) {
            bad = Err(Error::NoMemory);
            break;
        }
    }

    if (Task<void> c = close_fd(u32(fd.value())))
        co_await c;
    if (bad.is_err())
        co_return Err(bad.error());
    co_return move(out);
}

bool next_line(Str &rest, Str &line)
{
    if (rest.empty())
        return false;
    usize end = rest.find('\n');
    if (end == Str::npos) {
        line = rest;
        rest = Str();
        return true;
    }
    line = rest.substr(0, end);
    rest = rest.substr(end + 1);
    return true;
}

Str next_field(Str &line)
{
    usize at = 0;
    while (at < line.size() && line[at] == ' ')
        at++;
    usize end = at;
    while (end < line.size() && line[end] != ' ')
        end++;
    Str out = line.substr(at, end - at);
    line    = line.substr(end);
    return out;
}

namespace {

u64 wide(const u8 *p)
{
    return u64(sys_get_u32(p)) | (u64(sys_get_u32(p + 4)) << 32);
}

} // namespace

Task<Result<FileInfo>> stat_of(Str path)
{
    Result<SysReply> r = co_await sys_call(Sys::Stat, 0, path);
    if (r.is_err())
        co_return Err(r.error());
    if (r.value().data.size() < 12)
        co_return Err(Error::Io);

    const u8 *p = reinterpret_cast<const u8 *>(r.value().data.data());
    co_return FileInfo{ sys_get_u32(p), wide(p + 4) };
}

Task<Result<Vec<DirEntry>>> list_dir(Str path)
{
    Result<SysReply> r = co_await sys_call(Sys::List, 0, path);
    if (r.is_err())
        co_return Err(r.error());

    Str data = r.value().data;
    if (data.size() < 4)
        co_return Err(Error::Io);
    const u8 *p = reinterpret_cast<const u8 *>(data.data());
    usize n     = sys_get_u32(p);
    usize at    = 4;

    Vec<DirEntry> out;
    for (usize i = 0; i < n; i++) {
        if (at + 16 > data.size())
            co_return Err(Error::Io);
        DirEntry e;
        e.kind    = sys_get_u32(p + at);
        e.size    = wide(p + at + 4);
        usize len = sys_get_u32(p + at + 12);
        at += 16;
        if (at + len > data.size())
            co_return Err(Error::Io);
        if (!e.name.assign(Str(data.data() + at, len)) || !out.push(move(e)))
            co_return Err(Error::NoMemory);
        at += len;
    }
    co_return move(out);
}

Task<Result<void>> make_dir(Str path)
{
    Result<SysReply> r = co_await sys_call(Sys::MkDir, 0, path);
    if (r.is_err())
        co_return Err(r.error());
    co_return {};
}

Task<Result<void>> remove_path(Str path, bool all)
{
    Result<SysReply> r = co_await sys_call(Sys::Remove, all ? 1 : 0, path);
    if (r.is_err())
        co_return Err(r.error());
    co_return {};
}

// One operation for both, so the kernel answers "where am I now" whichever was
// asked. The reply is copied out: a Str into the waiter's buffer is only good
// until that slot's next syscall.
namespace {
Task<Result<String>> chdir(u32 arg, Str path)
{
    Result<SysReply> r = co_await sys_call(Sys::Chdir, arg, path);
    if (r.is_err())
        co_return Err(r.error());
    String out;
    if (!out.append(r.value().data))
        co_return Err(Error::NoMemory);
    co_return move(out);
}
} // namespace

Task<Result<String>> cwd_get()
{
    co_return co_await chdir(0, Str());
}

Task<Result<String>> cwd_set(Str path)
{
    co_return co_await chdir(1, path);
}

Task<Result<Piped>> make_pipe()
{
    Result<SysReply> r = co_await sys_call(Sys::Pipe, 0);
    if (r.is_err())
        co_return Err(r.error());
    if (r.value().data.size() < 8)
        co_return Err(Error::Io);
    const u8 *p = reinterpret_cast<const u8 *>(r.value().data.data());
    co_return Piped{ i32(sys_get_u32(p)), i32(sys_get_u32(p + 4)) };
}

Task<Result<u32>> spawn(Args v, ChildIo io)
{
    // Three descriptor words, then the argv blob — the same encoding _start is
    // entered with, so the kernel copies it across rather than rebuilding it.
    String payload;
    u8 head[SYS_SPAWN_HEAD * 4];
    sys_put_u32(head, io.in);
    sys_put_u32(head + 4, io.out);
    sys_put_u32(head + 8, io.err);
    usize n = argv_size(v.v.data(), v.size());
    if (!payload.append(Str(reinterpret_cast<const char *>(head), sizeof(head))) ||
        !payload.reserve(sizeof(head) + n))
        co_return Err(Error::NoMemory);
    for (usize i = 0; i < n; i++)
        payload.push(0);
    argv_encode(v.v.data(), v.size(), reinterpret_cast<u8 *>(payload.data()) + sizeof(head));

    Result<SysReply> r = co_await sys_call(Sys::Spawn, 0, payload.str());
    if (r.is_err())
        co_return Err(r.error());
    co_return u32(r.value().status);
}

Task<Result<Exited>> wait_child(u32 pid)
{
    Result<SysReply> r = co_await sys_call(Sys::Wait, pid);
    if (r.is_err())
        co_return Err(r.error());
    if (r.value().data.size() < 4)
        co_return Err(Error::Io);
    co_return Exited{ sys_get_u32(reinterpret_cast<const u8 *>(r.value().data.data())),
                      r.value().status };
}

Task<Result<void>> kill_child(u32 pid)
{
    Result<SysReply> r = co_await sys_call(Sys::Kill, pid);
    if (r.is_err())
        co_return Err(r.error());
    co_return {};
}

Task<Result<void>> set_fg(u32 pid)
{
    Result<SysReply> r = co_await sys_call(Sys::Fg, pid);
    if (r.is_err())
        co_return Err(r.error());
    co_return {};
}

// Both claims answer with the geometry and nothing else, so taking one and
// giving it back are the same two lines either way.
namespace {
Task<Result<Geometry>> claim(Sys op, bool take)
{
    Result<SysReply> r = co_await sys_call(op, take ? 1 : 0);
    if (r.is_err())
        co_return Err(r.error());
    if (r.value().data.size() < 8)
        co_return Err(Error::Io);
    const u8 *p = reinterpret_cast<const u8 *>(r.value().data.data());
    co_return Geometry{ sys_get_u32(p), sys_get_u32(p + 4) };
}
} // namespace

Task<Result<Geometry>> keys_claim(bool take)
{
    co_return co_await claim(Sys::KeyClaim, take);
}

Task<Result<Geometry>> screen_claim(bool take)
{
    co_return co_await claim(Sys::ScreenEnter, take);
}

Task<Result<KeyPress>> key_read()
{
    Result<SysReply> r = co_await sys_call(Sys::KeyRead, 0);
    if (r.is_err())
        co_return Err(r.error());
    if (r.value().data.size() < 16)
        co_return Err(Error::Io);
    const u8 *p = reinterpret_cast<const u8 *>(r.value().data.data());
    co_return KeyPress{ sys_get_u32(p), sys_get_u32(p + 4),
                        Geometry{ sys_get_u32(p + 8), sys_get_u32(p + 12) } };
}

namespace {
Task<Result<CursorAt>> cursor(u32 arg, Str payload)
{
    Result<SysReply> r = co_await sys_call(Sys::Cursor, arg, payload);
    if (r.is_err())
        co_return Err(r.error());
    if (r.value().data.size() < 20)
        co_return Err(Error::Io);
    const u8 *p = reinterpret_cast<const u8 *>(r.value().data.data());
    co_return CursorAt{ sys_get_u32(p), sys_get_u32(p + 4), sys_get_u32(p + 8) != 0,
                        Geometry{ sys_get_u32(p + 12), sys_get_u32(p + 16) } };
}
} // namespace

Task<Result<CursorAt>> cursor_get()
{
    co_return co_await cursor(0, Str());
}

Task<Result<CursorAt>> cursor_set(u32 x, u32 y, bool on)
{
    u8 head[12];
    sys_put_u32(head, x);
    sys_put_u32(head + 4, y);
    sys_put_u32(head + 8, on ? 1 : 0);
    co_return co_await cursor(1, Str(reinterpret_cast<const char *>(head), sizeof(head)));
}

Task<Result<void>> style_set(u8 fg, u8 bg, u8 attrs)
{
    Result<SysReply> r = co_await sys_call(Sys::Style, sys_style_pack(fg, bg, attrs));
    if (r.is_err())
        co_return Err(r.error());
    co_return {};
}

Task<Result<void>> sleep_for(u32 ms)
{
    u8 head[4];
    sys_put_u32(head, ms);
    Result<SysReply> r =
        co_await sys_call(Sys::Sleep, 0, Str(reinterpret_cast<const char *>(head), sizeof(head)));
    if (r.is_err())
        co_return Err(r.error());
    co_return {};
}

Task<Result<Clock>> clock_now()
{
    Result<SysReply> r = co_await sys_call(Sys::Clock, 0);
    if (r.is_err())
        co_return Err(r.error());
    if (r.value().data.size() < 12)
        co_return Err(Error::Io);

    const u8 *p = reinterpret_cast<const u8 *>(r.value().data.data());
    co_return Clock{ wide(p), i32(sys_get_u32(p + 8)) };
}

Task<Result<StorageInfo>> storage_of()
{
    Result<SysReply> r = co_await sys_call(Sys::Storage, 0);
    if (r.is_err())
        co_return Err(r.error());
    if (r.value().data.size() < 20)
        co_return Err(Error::Io);

    const u8 *p = reinterpret_cast<const u8 *>(r.value().data.data());
    u32 flags   = sys_get_u32(p + 16);
    StorageInfo out;
    out.quota     = wide(p);
    out.usage     = wide(p + 8);
    out.opfs      = flags & SYS_STORE_OPFS;
    out.sync      = flags & SYS_STORE_SYNC;
    out.persisted = flags & SYS_STORE_PERSISTED;
    out.known     = flags & SYS_STORE_KNOWN;
    co_return out;
}

namespace {

// A payload of `u32 length, the first string, the second` — what Fetch and
// Save both take, since neither can be told where one ends otherwise.
bool pack_pair(String &out, Str first, Str second)
{
    u8 head[4];
    sys_put_u32(head, u32(first.size()));
    return out.append(Str(reinterpret_cast<const char *>(head), sizeof(head))) &&
           out.append(first) && out.append(second);
}

} // namespace

Task<Result<Fetched>> fetch_url(Str url, Str spec)
{
    String req;
    if (!pack_pair(req, url, spec))
        co_return Err(Error::NoMemory);

    Result<SysReply> r = co_await sys_call(Sys::Fetch, 0, req.str());
    if (r.is_err())
        co_return Err(r.error());
    if (r.value().data.size() < 4)
        co_return Err(Error::Io);

    Fetched out;
    out.body   = r.value().status;
    out.status = sys_get_u32(reinterpret_cast<const u8 *>(r.value().data.data()));
    if (!out.headers.assign(r.value().data.substr(4)))
        co_return Err(Error::NoMemory);
    co_return move(out);
}

Task<Result<i32>> ws_connect(Str url)
{
    Result<SysReply> r = co_await sys_call(Sys::WsOpen, 0, url);
    if (r.is_err())
        co_return Err(r.error());
    co_return r.value().status;
}

Task<Result<String>> clip_get(bool wait)
{
    Result<SysReply> r = co_await sys_call(Sys::ClipRead, wait ? 1 : 0);
    if (r.is_err())
        co_return Err(r.error());

    String s;
    if (!s.assign(r.value().data))
        co_return Err(Error::NoMemory);
    co_return move(s);
}

Task<Result<void>> clip_put(Str text)
{
    Result<SysReply> r = co_await sys_call(Sys::ClipWrite, 0, text);
    if (r.is_err())
        co_return Err(r.error());
    co_return {};
}

Task<Result<Chosen>> pick()
{
    Result<SysReply> r = co_await sys_call(Sys::Pick, 0);
    if (r.is_err())
        co_return Err(r.error());

    Str data = r.value().data;
    if (data.size() < 4)
        co_return Err(Error::Io);
    const u8 *p = reinterpret_cast<const u8 *>(data.data());

    Chosen out;
    out.set  = r.value().status;
    usize n  = sys_get_u32(p);
    usize at = 4;
    for (usize i = 0; i < n; i++) {
        if (at + 4 > data.size())
            co_return Err(Error::Io);
        usize len = sys_get_u32(p + at);
        at += 4;
        if (at + len > data.size())
            co_return Err(Error::Io);
        String name;
        if (!name.assign(Str(data.data() + at, len)) || !out.names.push(move(name)))
            co_return Err(Error::NoMemory);
        at += len;
    }
    co_return move(out);
}

Task<Result<i32>> pick_open(const Chosen &c, usize index)
{
    u8 head[4];
    sys_put_u32(head, u32(index));
    Result<SysReply> r = co_await sys_call(Sys::PickOpen, u32(c.set),
                                           Str(reinterpret_cast<const char *>(head), sizeof(head)));
    if (r.is_err())
        co_return Err(r.error());
    co_return r.value().status;
}

Task<Result<void>> save(Str name, Str bytes)
{
    String req;
    if (!pack_pair(req, name, bytes))
        co_return Err(Error::NoMemory);

    Result<SysReply> r = co_await sys_call(Sys::Save, 0, req.str());
    if (r.is_err())
        co_return Err(r.error());
    co_return {};
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

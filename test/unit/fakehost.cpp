#include "fakehost.h"

#include "kernel/sysabi.h"
#include "kernel/traits.h"
#include "svc/svc.h"

bool FakeHost::file(Str path, Str text)
{
    Entry e;
    if (!e.key.assign(path) || !e.text.assign(text))
        return false;
    return files_.push(move(e));
}

bool FakeHost::route(Str url, Str body, u32 status)
{
    Entry e;
    if (!e.key.assign(url) || !e.text.assign(body))
        return false;
    e.status = status;
    return routes_.push(move(e));
}

Task<Result<u64>> FakeHost::now()
{
    co_return clock;
}

Task<Result<String>> FakeHost::load(Str path)
{
    for (const Entry &e : files_)
        if (e.key.str() == path) {
            String out;
            if (!out.assign(e.text.str()))
                co_return Err(Error::NoMemory);
            co_return move(out);
        }
    co_return Err(Error::NotFound);
}

Task<Result<i32>> FakeHost::open(Str url, u32 &status)
{
    for (usize i = 0; i < routes_.size(); i++) {
        if (routes_[i].key.str() != url)
            continue;
        status = routes_[i].status;
        if (!bodies_.push(Body{ i, 0, true }))
            co_return Err(Error::NoMemory);
        opened++;
        co_return i32(bodies_.size() - 1);
    }
    co_return Err(Error::Io);
}

Task<Result<String>> FakeHost::read(i32 body)
{
    if (body < 0 || usize(body) >= bodies_.size() || !bodies_[usize(body)].live)
        co_return Err(Error::Invalid);

    Body &b  = bodies_[usize(body)];
    Str text = routes_[b.entry].text.str();
    if (b.at >= text.size())
        co_return Err(Error::Closed);

    usize n = text.size() - b.at;
    if (n > SYS_CHUNK)
        n = SYS_CHUNK;
    String out;
    if (!out.assign(text.substr(b.at, n)))
        co_return Err(Error::NoMemory);
    b.at += n;
    co_return move(out);
}

Task<void> FakeHost::close(i32 body)
{
    if (body >= 0 && usize(body) < bodies_.size() && bodies_[usize(body)].live) {
        bodies_[usize(body)].live = false;
        closed++;
    }
    co_return;
}

Task<Result<bool>> FakeHost::verify(Str key, Str sig, Str bytes)
{
    if (no_ed25519)
        co_return Err(Error::Unsupported);

    Task<Result<void>> t = svc_verify(key, sig, bytes);
    if (!t)
        co_return Err(Error::NoMemory);
    Result<void> r = co_await t;
    if (r.is_err())
        co_return r.error() == Error::Perm ? Result<bool>(false) : Result<bool>(Err(r.error()));
    co_return true;
}

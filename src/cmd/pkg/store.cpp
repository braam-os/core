#include "store.h"

#include "proc/io.h"

namespace {

// ln -sf: what stands there goes, unfollowed, and nothing to remove is fine.
Task<Result<void>> relink(Str target, Str path)
{
    if (Task<Result<void>> t = remove_path(path, false))
        (void)co_await t;
    if (Task<Result<void>> t = make_link(target, path))
        co_return co_await t;
    co_return Err(Error::NoMemory);
}

} // namespace

Task<Result<void>> store_write(Str path, Str bytes)
{
    Task<Result<i32>> op = open_at(path, SYS_O_WRITE | SYS_O_CREATE | SYS_O_TRUNC);
    if (!op)
        co_return Err(Error::NoMemory);
    Result<i32> fd = co_await op;
    if (fd.is_err())
        co_return Err(fd.error());

    Result<void> w = Err(Error::NoMemory);
    if (Task<Result<void>> t = write_all(u32(fd.value()), bytes))
        w = co_await t;
    if (Task<void> c = close_fd(u32(fd.value())))
        co_await c;
    co_return w;
}

Task<Result<void>> store_perform(Span<const StoreOp> ops)
{
    for (const StoreOp &op : ops) {
        Result<void> r = Err(Error::NoMemory);
        switch (op.kind) {
        case StoreOpKind::MkDir:
            if (Task<Result<void>> t = make_dir_all(op.path.str()))
                r = co_await t;
            break;
        case StoreOpKind::Write:
            if (Task<Result<void>> t = store_write(op.path.str(), op.data.str()))
                r = co_await t;
            break;
        case StoreOpKind::Link:
            if (Task<Result<void>> t = relink(op.data.str(), op.path.str()))
                r = co_await t;
            break;
        case StoreOpKind::Rename:
            // No copy fallback: both names are in one directory and the source
            // is a symlink, so Err(Unsupported) here is a failure.
            if (Task<Result<void>> t = rename_path(op.path.str(), op.data.str()))
                r = co_await t;
            break;
        case StoreOpKind::Remove:
            if (Task<Result<void>> t = remove_path(op.path.str(), true))
                r = co_await t;
            if (r.is_err() && r.error() == Error::NotFound)
                r = {};
            break;
        }
        if (r.is_err())
            co_return r;
    }
    co_return {};
}

Task<Result<u32>> store_active()
{
    Result<String> target = Err(Error::NoMemory);
    if (Task<Result<String>> t = read_link(PKG_ACTIVE))
        target = co_await t;
    if (target.is_err())
        co_return target.error() == Error::NotFound ? Result<u32>(0) : Err(target.error());
    co_return gen_of(target.value().str());
}

Task<Result<String>> store_slurp(Str path)
{
    Result<String> r = Err(Error::NoMemory);
    if (Task<Result<String>> t = read_file(path))
        r = co_await t;
    if (r.is_err() && r.error() == Error::NotFound)
        co_return String();
    co_return r;
}

Task<Result<Vec<String>>> store_commands(Str name, Str version)
{
    String dir;
    if (!pkg_store_dir(name, version, "bin", dir))
        co_return Err(Error::NoMemory);

    Result<Vec<DirEntry>> got = Err(Error::NoMemory);
    if (Task<Result<Vec<DirEntry>>> t = list_dir(dir.str()))
        got = co_await t;

    Vec<String> out;
    if (got.is_err()) {
        if (got.error() == Error::NotFound || got.error() == Error::NotDir)
            co_return move(out);
        co_return Err(got.error());
    }
    for (DirEntry &e : got.value()) {
        if (e.kind == SYS_KIND_DIR)
            continue;
        if (!out.push(move(e.name)))
            co_return Err(Error::NoMemory);
    }
    co_return move(out);
}

Task<Result<void>> store_db_put(const DbRecord &r)
{
    String path, text;
    if (!pkg_db_file(r.pkg.name, r.pkg.version, path) || !db_write(r, text))
        co_return Err(Error::NoMemory);
    if (Task<Result<void>> t = store_write(path.str(), text.str()))
        co_return co_await t;
    co_return Err(Error::NoMemory);
}

Task<Result<String>> store_db_get(Str name, Str version)
{
    String path;
    if (!pkg_db_file(name, version, path))
        co_return Err(Error::NoMemory);
    if (Task<Result<String>> t = read_file(path.str()))
        co_return co_await t;
    co_return Err(Error::NoMemory);
}

Task<Result<void>> store_drop(Str name, Str version)
{
    String dir;
    if (!pkg_store_dir(name, version, "", dir))
        co_return Err(Error::NoMemory);
    if (Task<Result<void>> t = remove_path(dir.str(), true))
        co_return co_await t;
    co_return Err(Error::NoMemory);
}

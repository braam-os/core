#include "fs/path.h"
#include "proc/io.h"
#include "proc/opt.h"

namespace {

struct Flags {
    bool force = false; // -f
    bool ask   = false; // -i
};

// One file's bytes, through the syscalls: fimport.cpp's loop.
Task<Result<void>> copy_file(Str from, Str to)
{
    Result<i32> in = Err(Error::NoMemory);
    if (Task<Result<i32>> t = open_read(from))
        in = co_await t;
    if (in.is_err())
        co_return Err(in.error());

    Result<i32> out = Err(Error::NoMemory);
    if (Task<Result<i32>> t = open_at(to, SYS_O_WRITE | SYS_O_CREATE | SYS_O_TRUNC))
        out = co_await t;
    if (out.is_err()) {
        if (Task<void> k = close_fd(u32(in.value())))
            co_await k;
        co_return Err(out.error());
    }

    Result<void> r;
    for (;;) {
        Result<String> chunk = Err(Error::NoMemory);
        if (Task<Result<String>> t = read_chunk(u32(in.value())))
            chunk = co_await t;
        if (chunk.is_err()) {
            if (chunk.error() != Error::Closed)
                r = Err(chunk.error());
            break;
        }
        if (Result<void> w = co_await write_all(u32(out.value()), chunk.value().str());
            w.is_err()) {
            r = Err(w.error());
            break;
        }
    }

    if (Task<void> k = close_fd(u32(in.value())))
        co_await k;
    if (Task<void> k = close_fd(u32(out.value())))
        co_await k;
    co_return r;
}

// A whole tree, with an explicit stack rather than recursion — ls -R's shape,
// since a deep tree must not be a deep chain of coroutine frames. It descends
// on SYS_KIND_DIR alone, so a link is recreated rather than followed and no
// cycle guard is needed.
Task<Result<void>> copy_tree(Str from, Str to)
{
    Result<void> made = Err(Error::NoMemory);
    if (Task<Result<void>> t = make_dir(to))
        made = co_await t;
    if (made.is_err())
        co_return made;

    Vec<String> stack; // directories still to walk, relative to `from`
    if (!stack.push(String()))
        co_return Err(Error::NoMemory);

    while (!stack.empty()) {
        String rel = move(stack[stack.size() - 1]);
        stack.pop();

        String dir;
        if (!dir.assign(from) || !dir.append(rel.str()))
            co_return Err(Error::NoMemory);

        Result<Vec<DirEntry>> got = Err(Error::NoMemory);
        if (Task<Result<Vec<DirEntry>>> t = list_dir(dir.str()))
            got = co_await t;
        if (got.is_err())
            co_return Err(got.error());

        for (const DirEntry &e : got.value()) {
            String child, src, dst;
            if (!child.assign(rel.str()) || !child.push('/') || !child.append(e.name.str()) ||
                !src.assign(from) || !src.append(child.str()) || !dst.assign(to) ||
                !dst.append(child.str()))
                co_return Err(Error::NoMemory);

            Result<void> one = Err(Error::NoMemory);
            if (e.kind == SYS_KIND_DIR) {
                if (Task<Result<void>> t = make_dir(dst.str()))
                    one = co_await t;
                if (one.is_ok() && !stack.push(move(child)))
                    one = Err(Error::NoMemory);
            } else if (e.kind == SYS_KIND_LINK) {
                Result<String> target = Err(Error::NoMemory);
                if (Task<Result<String>> t = read_link(src.str()))
                    target = co_await t;
                if (target.is_err())
                    one = Err(target.error());
                else if (Task<Result<void>> t = make_link(target.value().str(), dst.str()))
                    one = co_await t;
            } else if (Task<Result<void>> t = copy_file(src.str(), dst.str())) {
                one = co_await t;
            }
            if (one.is_err())
                co_return one;
        }
    }
    co_return {};
}

// "overwrite <dst>? " and one line of the answer. BSD's rule: anything but a
// leading y declines.
Task<Result<bool>> confirm(Str dst, LineReader &answers)
{
    co_await write_all(SYS_STDERR, "overwrite ");
    co_await write_all(SYS_STDERR, dst);
    co_await write_all(SYS_STDERR, "? ");

    String line;
    Result<bool> r = Err(Error::NoMemory);
    if (Task<Result<bool>> t = answers.next(line))
        r = co_await t;
    if (r.is_err())
        co_return Err(r.error());
    co_return r.value() && !line.empty() && (line.str()[0] == 'y' || line.str()[0] == 'Y');
}

// One source to one destination, both absolute. The rename is tried first and
// Err(Unsupported) is what says to copy instead (proc/io.h).
Task<Result<void>> move_one(Str from, Str to, Flags f, LineReader &answers)
{
    // rename(2)'s no-op, checked here because the fallback below removes the
    // destination — which for `mv a a` is the file about to be moved.
    if (from == to)
        co_return {};
    // proc_main reports this one; the guard stands because the fallback below
    // would remove a destination inside the source.
    if (path_under(from, to))
        co_return Err(Error::Invalid);

    Result<FileInfo> src = Err(Error::NoMemory);
    if (Task<Result<FileInfo>> t = stat_of(from, false))
        src = co_await t;
    if (src.is_err())
        co_return Err(src.error());

    Result<FileInfo> dst = Err(Error::NoMemory);
    if (Task<Result<FileInfo>> t = stat_of(to, false))
        dst = co_await t;
    if (dst.is_err() && dst.error() != Error::NotFound)
        co_return Err(dst.error());

    if (dst.is_ok() && !f.force && f.ask) {
        Result<bool> yes = Err(Error::NoMemory);
        if (Task<Result<bool>> t = confirm(to, answers))
            yes = co_await t;
        if (yes.is_err())
            co_return Err(yes.error());
        if (!yes.value())
            co_return {};
    }

    Result<void> r = Err(Error::NoMemory);
    if (Task<Result<void>> t = rename_path(from, to))
        r = co_await t;
    if (r.is_ok() || r.error() != Error::Unsupported)
        co_return r;

    // A rename replaces, so the copy needs a clean name first. Nothing puts
    // the destination back if what follows fails.
    if (dst.is_ok()) {
        Result<void> d = Err(Error::NoMemory);
        if (Task<Result<void>> t = remove_path(to, true))
            d = co_await t;
        if (d.is_err())
            co_return d;
    }

    Result<void> c = Err(Error::NoMemory);
    if (src.value().kind == SYS_KIND_LINK) {
        Result<String> target = Err(Error::NoMemory);
        if (Task<Result<String>> t = read_link(from))
            target = co_await t;
        if (target.is_err())
            co_return Err(target.error());
        if (Task<Result<void>> t = make_link(target.value().str(), to))
            c = co_await t;
    } else if (src.value().kind == SYS_KIND_DIR) {
        if (Task<Result<void>> t = copy_tree(from, to))
            c = co_await t;
    } else if (Task<Result<void>> t = copy_file(from, to)) {
        c = co_await t;
    }
    if (c.is_err())
        co_return c;

    if (Task<Result<void>> t = remove_path(from, true))
        co_return co_await t;
    co_return Err(Error::NoMemory);
}

} // namespace

Task<i32> proc_main(Args args)
{
    Flags f;
    OptParse p(args, Opts{ "fi", "" });
    for (Opt o;;) {
        Result<bool> r = p.next(o);
        if (r.is_err()) {
            co_await write_all(SYS_STDERR, "mv: bad option\n");
            co_return 2;
        }
        if (!r.value())
            break;
        if (o.name == 'f')
            f.force = true;
        else
            f.ask = true;
    }

    Args rest = p.rest();
    if (rest.size() < 2) {
        co_await write_all(SYS_STDERR, "usage: mv [-fi] <src>... <dst>\n");
        co_return 2;
    }

    // Absolute throughout: move_one compares its two paths, and a relative one
    // would not compare.
    Result<String> here = Err(Error::NoMemory);
    if (Task<Result<String>> t = cwd_get())
        here = co_await t;
    if (here.is_err())
        co_return here.error() == Error::Cancelled ? 130 : 1;

    String dest;
    if (path_resolve(here.value().str(), rest[rest.size() - 1], dest).is_err())
        co_return 1;

    // With one source the last operand is the new name; with more it is the
    // directory they go in. Followed, as BSD's stat is.
    Result<FileInfo> s = Err(Error::NoMemory);
    if (Task<Result<FileInfo>> t = stat_of(dest.str(), true))
        s = co_await t;
    if (s.is_err() && s.error() == Error::Cancelled)
        co_return 130;

    bool into_dir = s.is_ok() && s.value().kind == SYS_KIND_DIR;
    if (!into_dir && rest.size() > 2) {
        co_await write_all(SYS_STDERR, "usage: mv [-fi] <src>... <dir>\n");
        co_return 2;
    }

    Input replies(Args{}, SYS_STDIN, "mv");
    LineReader answers(replies);

    i32 status = 0;
    for (usize i = 0; i + 1 < rest.size(); i++) {
        String from, to;
        if (path_resolve(here.value().str(), rest[i], from).is_err())
            co_return 1;
        if (into_dir) {
            if (path_join(dest.str(), path_basename(from.str()), to).is_err())
                co_return 1;
        } else if (!to.assign(dest.str())) {
            co_return 1;
        }

        if (from.str() != to.str() && path_under(from.str(), to.str())) {
            co_await write_all(SYS_STDERR, "mv: ");
            co_await write_all(SYS_STDERR, rest[i]);
            co_await write_all(SYS_STDERR, ": cannot move a directory into itself\n");
            status = 1;
            continue;
        }

        Result<void> r = Err(Error::NoMemory);
        if (Task<Result<void>> t = move_one(from.str(), to.str(), f, answers))
            r = co_await t;
        if (r.is_ok())
            continue;
        if (r.error() == Error::Cancelled)
            co_return 130;

        status = 1;
        if (Task<void> e = errln("mv", rest[i], r.error()))
            co_await e;
    }
    co_return status;
}

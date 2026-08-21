#include "db.h"
#include "host.h"
#include "index.h"
#include "kernel/fmt.h"
#include "kernel/traits.h"
#include "pkg.h"
#include "proc/io.h"
#include "store.h"

// Fetch, check, record: §7's steps behind one word. index_check writes
// nothing, so the store is touched only once every step has passed.

namespace {

constexpr Str USAGE = "Usage: pkg update\n";
constexpr Str NONE  = "pkg: no repositories; put one URL in /pkg/repositories\n";
constexpr Str MANY  = "pkg: /pkg/repositories: one repository, for now\n";

Task<Result<void>> put(u32 fd, Str text)
{
    co_return co_await write_all(fd, text);
}

Task<Result<void>> sayln(Str text)
{
    if (Task<Result<void>> t = put(SYS_STDOUT, text))
        CO_TRY_VOID(co_await t);
    co_return co_await put(SYS_STDOUT, "\n");
}

Task<Result<void>> record(const CheckedIndex &c)
{
    Vec<StoreOp> ops;
    if (!pkg_tree_ops(ops))
        co_return Err(Error::NoMemory);

    StoreOp op;
    op.kind = StoreOpKind::Write;
    if (!op.path.assign(PKG_INDEX) || !op.data.assign(c.text.str()) || !ops.push(move(op)))
        co_return Err(Error::NoMemory);

    if (Task<Result<void>> t = store_perform(ops))
        co_return co_await t;
    co_return Err(Error::NoMemory);
}

} // namespace

Task<i32> pkg_update(Args args)
{
    if (args.size() != 1) {
        if (Task<Result<void>> t = put(SYS_STDERR, USAGE))
            co_await t;
        co_return 2;
    }

    Result<String> text = Err(Error::NoMemory);
    if (Task<Result<String>> t = store_slurp(PKG_REPOS))
        text = co_await t;
    if (text.is_err()) {
        if (Task<void> e = errln("pkg", PKG_REPOS, text.error()))
            co_await e;
        co_return 1;
    }

    Vec<Str> repos;
    if (!repos_read(text.value().str(), repos)) {
        if (Task<void> e = errln("pkg", PKG_REPOS, Error::NoMemory))
            co_await e;
        co_return 1;
    }
    if (repos.empty() || repos.size() > 1) {
        if (Task<Result<void>> t = put(SYS_STDERR, repos.empty() ? NONE : MANY))
            co_await t;
        co_return 1;
    }

    Str repo = repo_trim(repos[0]);
    if (Task<Result<void>> t = sayln(repo))
        co_await t;

    CheckedIndex c;
    IndexStep step = IndexStep::Clock;
    Result<void> r = Err(Error::NoMemory);
    if (Task<Result<void>> t = index_check(pkg_host(), repo, c, step))
        r = co_await t;
    if (r.is_err()) {
        if (r.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("pkg", index_step_name(step), r.error()))
            co_await e;
        co_return 1;
    }

    if (!c.unchanged) {
        Result<void> w = Err(Error::NoMemory);
        if (Task<Result<void>> t = record(c))
            w = co_await t;
        if (w.is_err()) {
            if (Task<void> e = errln("pkg", PKG_INDEX, w.error()))
                co_await e;
            co_return 1;
        }
    }

    Buf<64> line;
    line.put("index ").put(c.head.version);
    if (c.unchanged)
        line.put(", unchanged");
    else
        line.put(", ")
            .put(c.packages.size())
            .put(c.packages.size() == 1 ? " package" : " packages");
    if (Task<Result<void>> t = sayln(line.str()))
        co_await t;
    co_return 0;
}

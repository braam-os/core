#include "condrun.h"

#include "cond.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/vec.h"
#include "proc/io.h"

namespace {

// What a program's first four bytes are, or the line that names one instead. A
// file is executable here exactly when the kernel would instantiate it
// (exec_meta and exec_shebang, kernel/sysabi.h); one chunk sees both.
constexpr Str WASM_MAGIC = Str("\0asm", 4);

Task<bool> answer(const CondProbe &p)
{
    if (p.op == 't') {
        Option<u32> fd          = parse_u32(p.arg);
        Task<Result<TtyInfo>> t = tty_of(fd ? fd.value() : 0);
        if (!t)
            co_return false;
        Result<TtyInfo> r = co_await t;
        co_return r.is_ok() && r.value().console;
    }

    if (p.op == 'w') {
        // v7 opens the file and closes it again rather than asking about a
        // mode, and here that is the only question there is: OPFS stores no
        // permissions, so what refuses a write is the mount (../fs/vfs.cpp).
        // A directory answers false, as it does in v7.
        Task<Result<i32>> t = open_at(p.arg, SYS_O_WRITE);
        if (!t)
            co_return false;
        Result<i32> r = co_await t;
        if (r.is_err())
            co_return false;
        co_await close_fd(u32(r.value()));
        co_return true;
    }

    if (p.op == 'x') {
        Task<bool> t = file_runnable(p.arg);
        co_return t ? co_await t : false;
    }

    if (p.op == 'n' || p.op == 'o') {
        // v7 has neither; both are false when either file is missing, which is
        // also what a filesystem keeping no mtime answers.
        Result<FileInfo> a = Err(Error::NotFound), b = Err(Error::NotFound);
        if (Task<Result<FileInfo>> t = stat_of(p.arg))
            a = co_await t;
        if (Task<Result<FileInfo>> t = stat_of(p.arg2))
            b = co_await t;
        if (a.is_err() || b.is_err())
            co_return false;
        co_return p.op == 'n' ? a.value().mtime > b.value().mtime
                              : a.value().mtime < b.value().mtime;
    }

    // -h asks about the link, everything else about what it points at.
    Task<Result<FileInfo>> t = stat_of(p.arg, p.op != 'h');
    if (!t)
        co_return false;
    Result<FileInfo> r = co_await t;
    if (r.is_err())
        co_return false;

    switch (p.op) {
    case 'd':
        co_return r.value().kind == SYS_KIND_DIR;
    case 'f':
        co_return r.value().kind == SYS_KIND_FILE;
    case 'h':
        co_return r.value().kind == SYS_KIND_LINK;
    case 's':
        co_return r.value().size > 0;
    default:
        // -r. There are no file permissions, so existing is readable.
        co_return true;
    }
}

} // namespace

Task<bool> file_runnable(Str path)
{
    Task<Result<i32>> t = open_read(path);
    if (!t)
        co_return false;
    Result<i32> r = co_await t;
    if (r.is_err())
        co_return false;

    bool runnable = false;
    if (Task<Result<String>> c = read_chunk(u32(r.value()))) {
        Result<String> got = co_await c;
        Str interp, arg;
        runnable = got.is_ok() && (got.value().str().starts_with(WASM_MAGIC) ||
                                   exec_shebang(got.value().str(), interp, arg));
    }
    co_await close_fd(u32(r.value()));
    co_return runnable;
}

Task<i32> cond_run(Args a, u32 errfd)
{
    // Every primary, then every answer, then the grammar again over the two.
    // Eager rather than lazy because v7's `-a` and `-o` are the bitwise `&`
    // and `|`: both sides run whatever the other said.
    Vec<CondProbe> probes;
    Vec<u8> answers;
    if (!cond_probes(a, probes)) {
        co_await write_all(errfd, "test: out of memory\n");
        co_return 2;
    }

    for (const CondProbe &p : probes) {
        Task<bool> t = answer(p);
        bool v       = t ? co_await t : false;
        if (!answers.push(v ? 1 : 0)) {
            co_await write_all(errfd, "test: out of memory\n");
            co_return 2;
        }
    }

    bool value = false;
    CondErr err;
    if (cond_eval(a, Span<const u8>(answers.data(), answers.size()), value, err))
        co_return value ? 0 : 1;

    String why;
    if (why.assign("test: ") && why.append(err.message) && why.push('\n'))
        co_await write_all(errfd, why.str());
    co_return 2;
}

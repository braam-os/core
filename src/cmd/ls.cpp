#include "fs/path.h"
#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/text.h"
#include "kernel/traits.h"
#include "proc/io.h"
#include "proc/opt.h"

// A listing, laid out for whatever stdout turns out to be: columns on the
// terminal, one name per line into a pipe. The trailing slash on a directory is
// not optional dressing — it is the only thing distinguishing a directory from
// a file in the short form.

namespace {

constexpr Opts LS_OPTS{ "1CRSdhlr", "" };

constexpr usize GAP         = 2;   // between columns
constexpr u32 WIDTH_DEFAULT = 80;  // -C with no terminal to measure
constexpr u64 BLOCK         = 512; // FS_BLOCK, which is what `total` counts
constexpr Str USAGE         = "usage: ls [-1CRSdhlr] [<path>...]\n";

// Everything that outlives an await. A coroutine frame past 512 bytes costs a
// whole 64 KiB span, so the entries, the recursion and the row being built live
// in one heap block instead.
struct Lister {
    bool detail  = false; // -l
    bool columns = false; // -C, or a terminal with no layout flag given
    bool human   = false; // -h
    bool recurse = false; // -R
    bool reverse = false; // -r
    bool by_size = false; // -S
    bool self    = false; // -d
    bool head    = false; // print `path:` before each directory

    u32 width = WIDTH_DEFAULT;

    Vec<DirEntry> ents; // the block being printed
    Vec<String> dirs;   // the directory operands, in the order given
    Vec<String> stack;  // -R: directories still to list, deepest last
    String row;         // one line, reused
    bool wrote = false; // has anything gone out, for the blank line before a header
};

// Cells, not bytes: the grid puts one codepoint in one cell, so Buf's put_left
// and put_right — which pad by size() — cannot be used on a name.
usize disp_width(Str s)
{
    usize n = 0;
    for (usize i = 0; i < s.size();) {
        char32_t ch = 0;
        usize k     = utf8_decode(s, i, ch);
        i += k ? k : 1;
        n++;
    }
    return n;
}

usize entry_width(const DirEntry &e)
{
    return disp_width(e.name.str()) + (e.kind == SYS_KIND_DIR ? 1 : 0);
}

bool put_name(String &out, const DirEntry &e)
{
    if (!out.append(e.name.str()))
        return false;
    return e.kind != SYS_KIND_DIR || out.push('/');
}

bool pad(String &out, usize n)
{
    for (usize i = 0; i < n; i++)
        if (!out.push(' '))
            return false;
    return true;
}

// 10G, 121M, 512B — one decimal below ten, as df prints it.
void human(Buf<32> &b, u64 bytes)
{
    constexpr Str UNIT[] = { "B", "K", "M", "G", "T", "P" };
    usize u              = 0;
    u64 whole = bytes, rem = 0;
    while (whole >= 1024 && u + 1 < sizeof(UNIT) / sizeof(UNIT[0])) {
        rem = whole % 1024;
        whole /= 1024;
        u++;
    }
    b.put(whole);
    if (u && whole < 10) {
        u64 tenth = (rem * 10) / 1024;
        if (tenth)
            b.put('.').put(tenth);
    }
    b.put(UNIT[u]);
}

void put_size(Buf<32> &b, u64 bytes, bool human_sizes)
{
    if (human_sizes)
        human(b, bytes);
    else
        b.put(bytes);
}

// The default is name order, which vfs_list already delivered. -S is stable, so
// that order stays the tiebreak; -r reverses whatever came out.
void sort_block(Lister &st)
{
    Vec<DirEntry> &v = st.ents;
    if (st.by_size)
        for (usize i = 1; i < v.size(); i++)
            for (usize k = i; k > 0 && v[k].size > v[k - 1].size; k--)
                swap(v[k], v[k - 1]);
    if (st.reverse && v.size() > 1)
        for (usize i = 0, j = v.size() - 1; i < j; i++, j--)
            swap(v[i], v[j]);
}

// One shared column width, as BSD's is — not a width per column.
void plan(const Lister &st, usize n, usize &cols, usize &rows, usize &colw)
{
    usize w = 0;
    for (usize i = 0; i < n; i++)
        w = max(w, entry_width(st.ents[i]));

    colw = w + GAP;
    cols = (st.width + GAP) / colw; // the last column needs no trailing gap
    if (cols < 1)
        cols = 1;
    if (cols > n)
        cols = n;
    rows = (n + cols - 1) / cols;
    cols = (n + rows - 1) / rows; // shrink, so no column comes out empty
}

// What a failed write exits with: ^C stays 130, as it does everywhere else.
i32 write_bad(Error e)
{
    return e == Error::Cancelled ? 130 : 1;
}

Task<i32> emit_short(Lister &st)
{
    usize n = st.ents.size();
    if (!n)
        co_return 0;
    usize cols = 1, rows = n, colw = 0;
    if (st.columns)
        plan(st, n, cols, rows, colw);

    // Column-major: a sorted listing is read down a column.
    for (usize r = 0; r < rows; r++) {
        st.row.clear();
        for (usize c = 0; c < cols; c++) {
            usize i = c * rows + r;
            if (i >= n)
                break;
            if (!put_name(st.row, st.ents[i]))
                co_return 1;
            // The last cell of a row is not padded: trailing blanks would put
            // the cursor in the deferred-wrap column.
            if (i + rows < n && !pad(st.row, colw - entry_width(st.ents[i])))
                co_return 1;
        }
        if (!st.row.push('\n'))
            co_return 1;
        if (Result<void> w = co_await write_all(SYS_STDOUT, st.row.str()); w.is_err())
            co_return write_bad(w.error());
    }
    co_return 0;
}

Task<i32> emit_long(Lister &st, bool with_total)
{
    // The size column is as wide as this block needs, and `total` counts
    // 512-byte blocks. A directory reports no size through this filesystem, so
    // it contributes none.
    u64 total = 0;
    usize w   = 1;
    for (const DirEntry &e : st.ents) {
        total += (e.size + BLOCK - 1) / BLOCK;
        Buf<32> t;
        put_size(t, e.size, st.human);
        w = max(w, t.str().size());
    }

    if (with_total) {
        Buf<32> t;
        t.put("total ").put(total).put('\n');
        if (Result<void> x = co_await write_all(SYS_STDOUT, t.str()); x.is_err())
            co_return write_bad(x.error());
    }

    for (const DirEntry &e : st.ents) {
        Buf<32> num;
        put_size(num, e.size, st.human);

        // A size is ASCII, so put_right's byte padding is cell padding here.
        Buf<64> t;
        t.put(e.kind == SYS_KIND_DIR ? Str("dir  ") : Str("file "));
        t.put_right(num.str(), w).put(' ');

        st.row.clear();
        if (!st.row.append(t.str()) || !put_name(st.row, e) || !st.row.push('\n'))
            co_return 1;
        if (Result<void> x = co_await write_all(SYS_STDOUT, st.row.str()); x.is_err())
            co_return write_bad(x.error());
    }
    co_return 0;
}

Task<i32> emit_block(Lister &st, bool with_total)
{
    if (st.ents.empty() && !with_total)
        co_return 0;
    st.wrote = true;

    Task<i32> t = st.detail ? emit_long(st, with_total) : emit_short(st);
    if (!t)
        co_return 1;
    co_return co_await t;
}

// A `path:` header, with a blank line before it once anything has been printed.
Task<i32> emit_head(Lister &st, Str path)
{
    st.row.clear();
    if (st.wrote && !st.row.push('\n'))
        co_return 1;
    if (!st.row.append(path) || !st.row.append(":\n"))
        co_return 1;
    if (Result<void> w = co_await write_all(SYS_STDOUT, st.row.str()); w.is_err())
        co_return write_bad(w.error());
    st.wrote = true;
    co_return 0;
}

// One directory: its entries, sorted, with -R's children pushed for later.
Task<i32> list_one(Lister &st, Str path)
{
    if (st.head) {
        Task<i32> h = emit_head(st, path);
        if (!h)
            co_return 1;
        if (i32 bad = co_await h)
            co_return bad;
    }

    Result<Vec<DirEntry>> r = Err(Error::NoMemory);
    if (Task<Result<Vec<DirEntry>>> t = list_dir(path))
        r = co_await t;
    if (r.is_err()) {
        if (r.error() == Error::Cancelled)
            co_return 130;
        if (Task<void> e = errln("ls", path, r.error()))
            co_await e;
        co_return 1;
    }

    st.ents = move(r.value());
    sort_block(st);

    // Pushed in reverse of the order they will print, so they pop in it.
    if (st.recurse)
        for (usize i = st.ents.size(); i > 0; i--) {
            const DirEntry &e = st.ents[i - 1];
            if (e.kind != SYS_KIND_DIR)
                continue;
            String child;
            if (path_join(path, e.name.str(), child).is_err() || !st.stack.push(move(child))) {
                if (Task<void> x = errln("ls", path, Error::NoMemory))
                    co_await x;
                co_return 1;
            }
        }

    Task<i32> t = emit_block(st, st.detail);
    if (!t)
        co_return 1;
    co_return co_await t;
}

// Every operand is stat'd up front. Files print as one block, then each
// directory in the order it was given — which is what puts `ls f d` in the
// order BSD puts it in.
Task<i32> run(Lister &st, Args paths)
{
    i32 status  = 0;
    usize count = paths.size();

    for (usize k = 0; k == 0 || k < count; k++) {
        Str p = count ? paths[k] : Str(".");

        Result<FileInfo> s = Err(Error::NoMemory);
        if (Task<Result<FileInfo>> t = stat_of(p))
            s = co_await t;
        if (s.is_err()) {
            if (s.error() == Error::Cancelled)
                co_return 130;
            status = 1;
            if (Task<void> e = errln("ls", p, s.error()))
                co_await e;
            continue;
        }

        bool dir = s.value().kind == SYS_KIND_DIR && !st.self;
        String name;
        if (!name.assign(p))
            co_return 1;
        bool ok = dir ? st.dirs.push(move(name))
                      : st.ents.push(DirEntry{ move(name), s.value().kind, s.value().size });
        if (!ok)
            co_return 1;
    }

    st.head = count > 1 || st.recurse;

    if (!st.ents.empty()) {
        sort_block(st);
        Task<i32> t = emit_block(st, false);
        if (!t)
            co_return 1;
        if (i32 bad = co_await t) {
            if (bad == 130)
                co_return 130;
            status = 1;
        }
    }

    for (usize k = 0; k < st.dirs.size(); k++) {
        if (!st.stack.push(move(st.dirs[k])))
            co_return 1;
        // Depth first: -R pushed this one's children while it was listed.
        while (!st.stack.empty()) {
            String p = move(st.stack[st.stack.size() - 1]);
            st.stack.pop();
            Task<i32> t = list_one(st, p.str());
            if (!t)
                co_return 1;
            if (i32 bad = co_await t) {
                if (bad == 130)
                    co_return 130;
                status = 1;
            }
        }
    }
    co_return status;
}

} // namespace

Task<i32> proc_main(Args args)
{
    Lister *st = heap_new<Lister>();
    if (!st) {
        co_await write_all(SYS_STDERR, "ls: out of memory\n");
        co_return 1;
    }
    struct Free {
        ~Free() { heap_delete(p); }
        Lister *p;
    } free_lister{ st };

    // The last of -l, -1 and -C wins, as it does in BSD.
    char layout = 0;
    OptParse opts(args, LS_OPTS);
    Opt o;
    for (;;) {
        Result<bool> more = opts.next(o);
        if (more.is_err()) {
            Buf<64> b;
            b.put("ls: illegal option -- ").put(o.name).put('\n');
            co_await write_all(SYS_STDERR, b.str());
            co_await write_all(SYS_STDERR, USAGE);
            co_return 2;
        }
        if (!more.value())
            break;
        switch (o.name) {
        case '1':
        case 'C':
        case 'l':
            layout = o.name;
            break;
        case 'R':
            st->recurse = true;
            break;
        case 'S':
            st->by_size = true;
            break;
        case 'd':
            st->self = true;
            break;
        case 'h':
            st->human = true;
            break;
        case 'r':
            st->reverse = true;
            break;
        }
    }
    // -d lists a directory as itself, so there is nothing below it to walk.
    if (st->self)
        st->recurse = false;

    // The one question a program cannot answer for itself.
    Result<TtyInfo> tty = Err(Error::Unsupported);
    if (Task<Result<TtyInfo>> t = tty_of(SYS_STDOUT))
        tty = co_await t;
    if (tty.is_err() && tty.error() == Error::Cancelled)
        co_return 130;

    bool console = tty.is_ok() && tty.value().console;
    st->detail   = layout == 'l';
    st->columns  = layout == 'C' || (!layout && console);
    if (console && tty.value().at.cols)
        st->width = tty.value().at.cols;

    Task<i32> t = run(*st, opts.rest());
    if (!t)
        co_return 1;
    co_return co_await t;
}

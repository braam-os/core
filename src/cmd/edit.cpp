// A full-screen, modeless editor: type to insert, ^S to save, ^Q to quit.
// There are no modes and no escape sequences on either side of it — keys are
// {code, mods} and the screen is an array (Concept.md §2.3).
//
// ^C is not the editor's to handle: the pump cancels the pipeline with it,
// which throws the buffer away, and that is deliberate — a program that has
// taken the whole screen must stay killable by the key that kills everything.
#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/text.h"
#include "kernel/traits.h"
#include "proc/screen.h"
#include "ui/textbuf.h"
#include "ui/view.h"

namespace {

// The whole of the editor's state, in a heap block: in the coroutine frame it
// would cost a 64 KiB span (Concept.md §8.2).
struct Editor {
    TextBuf buf;
    TextView view;
    String path;
    String scratch;      // serialize() writes here, so saving allocates once
    usize row  = 0;      // the cursor's line
    usize off  = 0;      // and its byte offset into that line
    usize want = 0;      // the column vertical movement aims for
    Str note;            // one transient line of status, cleared by the next key
    bool arming = false; // ^Q was pressed on a modified buffer
};

// Reads the file into the buffer. A missing file is a new one; a directory is
// refused before the screen is taken, when a diagnostic can still be seen.
Task<Result<void>> load(Editor &e)
{
    Task<Result<FileInfo>> st = stat_of(e.path.str());
    if (!st)
        co_return Err(Error::NoMemory);
    Result<FileInfo> s = co_await st;
    if (s.is_err()) {
        if (s.error() != Error::NotFound)
            co_return Err(s.error());
        co_return e.buf.load("");
    }
    if (s.value().kind == SYS_KIND_DIR)
        co_return Err(Error::IsDir);

    Task<Result<String>> rd = read_file(e.path.str());
    if (!rd)
        co_return Err(Error::NoMemory);
    Result<String> text = co_await rd;
    if (text.is_err())
        co_return Err(text.error());
    co_return e.buf.load(text.value().str());
}

Task<Result<void>> save(Editor &e)
{
    CO_TRY_VOID(e.buf.serialize(e.scratch));

    Task<Result<i32>> op = open_at(e.path.str(), SYS_O_WRITE | SYS_O_CREATE | SYS_O_TRUNC);
    if (!op)
        co_return Err(Error::NoMemory);
    Result<i32> fd = co_await op;
    if (fd.is_err())
        co_return Err(fd.error());

    Result<void> w = Err(Error::NoMemory);
    if (Task<Result<void>> t = write_all(u32(fd.value()), e.scratch.str()))
        w = co_await t;
    if (Task<void> c = close_fd(u32(fd.value())))
        co_await c;
    if (w.is_err())
        co_return Err(w.error());

    e.buf.clear_modified();
    co_return {};
}

void paint(Editor &e, ProcScreen &fs)
{
    Pane body = fs.body();
    usize col = e.buf.column(e.row, e.off);
    e.view.follow(e.row, col, body.height(), body.width());
    e.view.paint(body, e.buf);

    Buf<96> line;
    line.put(" ").put(e.path.str()).put(e.buf.modified() ? " *  " : "  ");
    line.put(e.row + 1).put(":").put(col + 1);
    if (!e.note.empty())
        line.put("  ").put(e.note);
    else
        line.put("  ^S saves, ^Q quits");

    Pane bar = fs.status();
    bar.style(COLOR_BLACK, COLOR_WHITE);
    bar.move(0, 0);
    bar.write(line.str());
    bar.fill_row();

    body.place_cursor(u32(col - e.view.left()), u32(e.row - e.view.top()));
}

// Vertical movement keeps the column it started from, so passing a short line
// does not lose the cursor's place — the same rule every editor has.
void go_row(Editor &e, usize row)
{
    e.row = row < e.buf.lines() ? row : e.buf.lines() - 1;
    e.off = e.buf.offset(e.row, e.want);
}

void set_want(Editor &e)
{
    e.want = e.buf.column(e.row, e.off);
}

} // namespace

Task<i32> proc_main(Args args)
{
    if (args.size() != 2) {
        co_await write_all(SYS_STDERR, "usage: edit <file>\n");
        co_return 2;
    }

    Editor *e = heap_new<Editor>();
    if (!e) {
        co_await write_all(SYS_STDERR, "edit: out of memory\n");
        co_return 1;
    }
    struct Free {
        ~Free() { heap_delete(e); }
        Editor *e;
    } free_editor{ e };

    if (!e->path.assign(args[1])) {
        co_await write_all(SYS_STDERR, "edit: out of memory\n");
        co_return 1;
    }

    if (Task<Result<void>> t = load(*e)) {
        Result<void> r = co_await t;
        if (r.is_err()) {
            if (Task<void> d = errln("edit", args[1], r.error()))
                co_await d;
            co_return 1;
        }
    }

    ProcScreen fs;
    if ((co_await fs.take_keys()).is_err()) {
        co_await write_all(SYS_STDERR, "edit: no keyboard\n");
        co_return 1;
    }
    if ((co_await fs.take_screen()).is_err()) {
        co_await write_all(SYS_STDERR, "edit: no screen\n");
        co_return 1;
    }
    fs.grid().cursor_on = true;

    for (;;) {
        paint(*e, fs);
        if ((co_await fs.flush()).is_err())
            co_return 1;

        Result<Key> r = co_await fs.next_key();
        if (r.is_err()) {
            if (r.error() == Error::Again)
                continue;
            co_return r.error() == Error::Cancelled ? 130 : 1;
        }

        Key k       = r.value();
        u32 h       = fs.body().height();
        usize lines = e->buf.lines();
        bool arming = e->arming;
        e->arming   = false;
        e->note     = Str();

        if (k.mods & MOD_CTRL) {
            if (k.code == 'q') {
                if (!e->buf.modified() || arming)
                    co_return 0;
                e->note   = "modified — ^Q again to discard";
                e->arming = true;
                continue;
            }
            if (k.code == 's') {
                Task<Result<void>> t = save(*e);
                if (!t) {
                    e->note = "out of memory";
                    continue;
                }
                Result<void> w = co_await t;
                if (w.is_err())
                    e->note = w.error() == Error::Cancelled ? "cancelled" : error_name(w.error());
                else
                    e->note = "written";
                continue;
            }
            continue;
        }

        switch (k.code) {
        case KEY_LEFT:
            if (e->off)
                e->off = e->buf.prev(e->row, e->off);
            else if (e->row) {
                e->row--;
                e->off = e->buf.line(e->row).size();
            }
            set_want(*e);
            break;
        case KEY_RIGHT:
            if (e->off < e->buf.line(e->row).size())
                e->off = e->buf.next(e->row, e->off);
            else if (e->row + 1 < lines) {
                e->row++;
                e->off = 0;
            }
            set_want(*e);
            break;
        case KEY_UP:
            if (e->row)
                go_row(*e, e->row - 1);
            break;
        case KEY_DOWN:
            if (e->row + 1 < lines)
                go_row(*e, e->row + 1);
            break;
        case KEY_PAGE_UP:
            go_row(*e, e->row > h ? e->row - h : 0);
            break;
        case KEY_PAGE_DOWN:
            go_row(*e, e->row + h);
            break;
        case KEY_HOME:
            e->off = 0;
            set_want(*e);
            break;
        case KEY_END:
            e->off = e->buf.line(e->row).size();
            set_want(*e);
            break;
        case KEY_ENTER:
            if (e->buf.split(e->row, e->off).is_err()) {
                e->note = "out of memory";
                break;
            }
            e->row++;
            e->off = 0;
            set_want(*e);
            break;
        case KEY_BACKSPACE:
            if (e->off) {
                usize at = e->buf.prev(e->row, e->off);
                e->buf.erase(e->row, at);
                e->off = at;
            } else if (e->row) {
                Result<usize> at = e->buf.join(e->row - 1);
                if (at.is_ok()) {
                    e->row--;
                    e->off = at.value();
                }
            }
            set_want(*e);
            break;
        case KEY_DELETE:
            if (e->off < e->buf.line(e->row).size())
                e->buf.erase(e->row, e->off);
            else if (e->row + 1 < lines)
                (void)e->buf.join(e->row);
            break;
        default:
            if (!k.printable() && k.code != KEY_TAB)
                break;
            char utf8[4];
            usize n = utf8_encode(k.code == KEY_TAB ? U' ' : k.code, utf8);
            if (e->buf.insert(e->row, e->off, Str(utf8, n)).is_err()) {
                e->note = "out of memory";
                break;
            }
            e->off += n;
            set_want(*e);
            break;
        }
    }
}

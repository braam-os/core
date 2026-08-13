// A pager. It reads its input to the end before it paints anything, which is
// not the laziness a real `less` has: CancelState::waiting is a single slot, so
// one task cannot be parked on a pipe and on the keyboard at the same time.
#include "kernel/alloc.h"
#include "kernel/fmt.h"
#include "kernel/key.h"
#include "kernel/screen.h"
#include "kernel/traits.h"
#include "ui/full.h"
#include "ui/textbuf.h"
#include "ui/view.h"
#include "user/io.h"
#include "user/prog.h"
#include "user/tty.h"

namespace {

// A heap block, not locals: a TextBuf and a TextView in the coroutine frame
// would push it past the allocator's top size class (Concept.md §8.2).
struct Pager {
    TextBuf buf;
    TextView view;
    String name;
};

void paint(Pager &p, const FullScreen &fs)
{
    Pane body = fs.body();
    p.view.paint(body, p.buf);

    usize lines = p.buf.lines();
    usize shown = min(lines, usize(p.view.top() + body.height()));
    u32 pct     = lines ? u32(shown * 100 / lines) : 100;

    Buf<80> line;
    line.put(" ").put(p.name.empty() ? Str("stdin") : p.name.str()).put("  ").put(pct).put("%  ");
    line.put(shown == lines ? "END  " : "").put("— q quits, arrows and PgUp/PgDn scroll");

    Pane bar = fs.status();
    bar.style(COLOR_BLACK, COLOR_WHITE);
    bar.move(0, 0);
    bar.write(line.str());
    bar.fill_row();
}

} // namespace

BRAAM_PROGRAM(prog_less, "less", "[file] — page through a file, or through stdin")
{
    if (args.size() > 2) {
        co_await io.err.write("usage: less [file]\n");
        co_return 2;
    }

    Pager *p = heap_new<Pager>();
    if (!p) {
        co_await io.err.write("less: out of memory\n");
        co_return 1;
    }
    struct Free {
        ~Free() { heap_delete(p); }
        Pager *p;
    } free_pager{ p };

    // Claimed before the input is read, so that anything typed while a slow
    // pipe fills up is queued for us rather than echoed at the shell.
    KeyInput keys;
    if (!keys.ok()) {
        co_await io.err.write("less: no keyboard\n");
        co_return 1;
    }

    Inputs files;
    if (i32 bad = co_await open_inputs(files, args.tail(), "less", io))
        co_return bad;
    if (args.size() == 2 && !p->name.assign(args[1]))
        co_return 1;

    LineReader in(input_of(files, io));
    String line;
    for (;;) {
        Result<bool> r = co_await in.next(line);
        if (r.is_err())
            co_return r.error() == Error::Cancelled ? 130 : 1;
        if (!r.value())
            break;
        if (p->buf.add(line.str()).is_err())
            co_return 1;
    }

    FullScreen fs;
    if (!fs.ok()) {
        co_await io.err.write("less: no screen\n");
        co_return 1;
    }

    for (;;) {
        paint(*p, fs);

        Result<Key> r = co_await keys.next();
        if (r.is_err()) {
            if (r.error() == Error::Again)
                continue;
            co_return r.error() == Error::Cancelled ? 130 : 1;
        }

        u32 h       = fs.body().height();
        u32 page    = h > 1 ? h - 1 : 1;
        usize lines = p->buf.lines();
        Key k       = r.value();

        switch (k.code) {
        case 'q':
        case 'Q':
        case KEY_ESCAPE:
            co_return 0;
        case KEY_UP:
        case 'k':
            p->view.scroll(-1, lines, h);
            break;
        case KEY_DOWN:
        case 'j':
        case KEY_ENTER:
            p->view.scroll(1, lines, h);
            break;
        case KEY_PAGE_UP:
        case 'b':
            p->view.scroll(-i32(page), lines, h);
            break;
        case KEY_PAGE_DOWN:
        case ' ':
        case 'f':
            p->view.scroll(i32(page), lines, h);
            break;
        case KEY_HOME:
        case 'g':
            p->view.to(0, lines, h);
            break;
        case KEY_END:
        case 'G':
            p->view.to(lines, lines, h);
            break;
        default:
            break;
        }
    }
}

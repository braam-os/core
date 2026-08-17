#include "screen.h"

#include "alloc.h"
#include "host.h"
#include "text.h"
#include "traits.h"

namespace {

// .bss, so there is no static initialisation for --no-entry to skip.
Screen g;
Cell *g_cells;

u8 g_fg = COLOR_WHITE;
u8 g_bg = COLOR_BLACK;
u8 g_attrs;

// One damage rectangle, half-open, valid only while g_dirty (Concept.md §3.5).
u32 g_x0, g_y0, g_x1, g_y1;
bool g_dirty;

// Where the renderer last drew the cursor, so flush can repaint the cell it
// left. Keeping this in one place is what stops a mutation added later from
// leaving a ghost behind.
u32 g_drawn_x, g_drawn_y;

// Rows the grid has moved up, ever. Nothing else counts them, and a writer
// holding an anchor has no other way to learn its row went with them.
u64 g_scrolled;

// The scrollback ring: rows that have left the top, at the grid's own width.
Cell *g_hist;
u32 g_hist_cols;
u32 g_hist_head;  // where the next row goes
u32 g_hist_count; // rows held, up to SCREEN_SCROLLBACK

// g_cells is always the live screen. While g_view_cells exists it is what
// g.cells points at, showing the grid from g_view rows further back.
Cell *g_view_cells;
u32 g_view;
bool g_view_dirty;  // recompose at the next flush
bool g_cursor_live; // g.cursor_on, which a view forces to 0

bool sized()
{
    return g_cells && g.cols && g.rows;
}

Cell blank()
{
    return Cell{ 0, g_fg, g_bg, g_attrs, 0 };
}

void damage(u32 x, u32 y)
{
    if (!g_dirty) {
        g_x0    = x;
        g_y0    = y;
        g_x1    = x + 1;
        g_y1    = y + 1;
        g_dirty = true;
        return;
    }
    g_x0 = min(g_x0, x);
    g_y0 = min(g_y0, y);
    g_x1 = max(g_x1, x + 1);
    g_y1 = max(g_y1, y + 1);
}

void damage_all()
{
    if (!sized())
        return;
    g_x0    = 0;
    g_y0    = 0;
    g_x1    = g.cols;
    g_y1    = g.rows;
    g_dirty = true;
}

void clear_row(Cell *cells, u32 cols, u32 y)
{
    Cell b = blank();
    for (u32 x = 0; x < cols; x++)
        cells[y * cols + x] = b;
}

void hist_drop()
{
    heap_free(g_hist);
    g_hist       = nullptr;
    g_hist_cols  = 0;
    g_hist_head  = 0;
    g_hist_count = 0;
}

// The k-th newest row, k = 1 being the one that left most recently.
const Cell *hist_row(u32 k)
{
    u32 at = (g_hist_head + SCREEN_SCROLLBACK - k) % SCREEN_SCROLLBACK;
    return g_hist + usize(at) * g_hist_cols;
}

// Keeps a row that is leaving the top, building the ring on first use.
void hist_push(const Cell *row)
{
    if (g_hist && g_hist_cols != g.cols)
        hist_drop(); // a width it cannot hold: the rewidth failed
    if (!g_hist) {
        g_hist = static_cast<Cell *>(heap_alloc(usize(SCREEN_SCROLLBACK) * g.cols * sizeof(Cell)));
        if (!g_hist)
            return; // no scrollback; the screen is otherwise unaffected
        g_hist_cols = g.cols;
    }
    __builtin_memcpy(g_hist + usize(g_hist_head) * g_hist_cols, row, g_hist_cols * sizeof(Cell));
    g_hist_head = (g_hist_head + 1) % SCREEN_SCROLLBACK;
    if (g_hist_count < SCREEN_SCROLLBACK)
        g_hist_count++;
}

// Carries the history across a width change, clipping or padding each row the
// way the grid itself is. Dropped if the new ring will not allocate.
void hist_rewidth(u32 cols)
{
    if (!g_hist || g_hist_cols == cols)
        return;

    Cell *next = static_cast<Cell *>(heap_alloc(usize(SCREEN_SCROLLBACK) * cols * sizeof(Cell)));
    if (!next) {
        hist_drop();
        return;
    }

    u32 keep = min(cols, g_hist_cols);
    for (u32 i = 0; i < g_hist_count; i++) { // oldest first, unwinding the ring
        Cell *dst = next + usize(i) * cols;
        clear_row(next, cols, i);
        __builtin_memcpy(dst, hist_row(g_hist_count - i), keep * sizeof(Cell));
    }

    heap_free(g_hist);
    g_hist      = next;
    g_hist_cols = cols;
    g_hist_head = g_hist_count % SCREEN_SCROLLBACK;
}

// Fills the block the renderer reads: rows above the live screen out of the
// ring, the rest out of the grid.
void view_compose()
{
    if (g_view > (g_hist ? g_hist_count : 0))
        g_view = g_hist ? g_hist_count : 0; // never index past the ring
    for (u32 i = 0; i < g.rows; i++) {
        Cell *dst = g_view_cells + usize(i) * g.cols;
        if (i < g_view)
            __builtin_memcpy(dst, hist_row(g_view - i), g.cols * sizeof(Cell));
        else
            __builtin_memcpy(dst, g_cells + usize(i - g_view) * g.cols, g.cols * sizeof(Cell));
    }
    g_view_dirty = false;
}

// Points the descriptor at a block of its own, leaving the live grid where it
// is for the writers.
bool view_open()
{
    if (g_view_cells)
        return true;
    g_view_cells = static_cast<Cell *>(heap_alloc(usize(g.cols) * g.rows * sizeof(Cell)));
    if (!g_view_cells)
        return false;
    g_cursor_live = g.cursor_on;
    g.cursor_on   = 0; // the cursor is on the live screen, not up here
    g.cells       = u32(reinterpret_cast<usize>(g_view_cells));
    return true;
}

void scroll()
{
    hist_push(g_cells); // the row about to be overwritten
    for (u32 y = 1; y < g.rows; y++)
        __builtin_memcpy(g_cells + (y - 1) * g.cols, g_cells + y * g.cols, g.cols * sizeof(Cell));
    clear_row(g_cells, g.cols, g.rows - 1);
    if (g_drawn_y)
        g_drawn_y--;
    g_scrolled++;

    // A view stays put: the offset grows with the live top, until the ring is
    // full and the clamp lets it drift. Composing is the flush's, since this
    // runs once per output line.
    if (g_view) {
        if (g_view < g_hist_count)
            g_view++;
        g_view_dirty = true;
    }
    damage_all();
}

// Down one row, scrolling when there is no row left.
void next_row()
{
    if (g.cursor_y + 1 < g.rows)
        g.cursor_y++;
    else
        scroll();
}

// The wrap is deferred: cursor_x sits at cols until the next character is
// written, so filling the last column does not scroll the screen on its own.
void wrap_pending()
{
    if (g.cursor_x >= g.cols) {
        g.cursor_x = 0;
        next_row();
    }
}

} // namespace

u32 screen_resize(u32 cols, u32 rows)
{
    cols = cols ? min(cols, u32(SCREEN_MAX_COLS)) : 1;
    rows = rows ? min(rows, u32(SCREEN_MAX_ROWS)) : 1;

    Cell *next = static_cast<Cell *>(heap_alloc(usize(cols) * rows * sizeof(Cell)));
    if (!next)
        return 0; // the old grid is still whole, and still the one on screen
    for (u32 y = 0; y < rows; y++)
        clear_row(next, cols, y);

    // After the bail: a resize that could not allocate leaves the view alone.
    screen_view_home();

    // Rows 0..cursor_y are the ones in use. Keep as many of them as fit,
    // dropping from the top, and land them at the top of the new grid — output
    // grows downwards from there, so that is where the eye already is.
    u32 live      = g.rows ? min(g.cursor_y + 1, g.rows) : 0;
    u32 keep_rows = min(rows, live);
    u32 keep_cols = min(cols, g.cols);
    if (g_cells && keep_rows && keep_cols) {
        u32 src0 = live - keep_rows;
        for (u32 i = 0; i < src0; i++)
            hist_push(g_cells + i * g.cols); // dropped from the top, so kept
        for (u32 i = 0; i < keep_rows; i++)
            __builtin_memcpy(next + i * cols, g_cells + (src0 + i) * g.cols,
                             keep_cols * sizeof(Cell));

        g.cursor_y = g.cursor_y - src0; // cursor_y is live - 1, so keep_rows - 1
        g_scrolled += src0;             // rows off the top, as a scroll's are
        if (g.cursor_x > cols)
            g.cursor_x = cols;
    } else {
        g.cursor_x = g.cursor_y = 0;
    }

    heap_free(g_cells);
    hist_rewidth(cols); // after the free, so the peak is one grid lower
    g_cells   = next;
    g.magic   = SCREEN_MAGIC;
    g.cols    = cols;
    g.rows    = rows;
    g.cells   = u32(reinterpret_cast<usize>(next));
    g_drawn_x = g.cursor_x;
    g_drawn_y = g.cursor_y;
    damage_all();
    return u32(reinterpret_cast<usize>(&g));
}

const Screen &screen()
{
    return g;
}

Cell *screen_cells()
{
    return g_cells;
}

u64 screen_scrolled()
{
    return g_scrolled;
}

void screen_style(u8 fg, u8 bg, u8 attrs)
{
    g_fg    = fg;
    g_bg    = bg;
    g_attrs = attrs;
}

void screen_put(char32_t ch)
{
    if (!sized())
        return;
    wrap_pending();
    g_cells[g.cursor_y * g.cols + g.cursor_x] = Cell{ ch, g_fg, g_bg, g_attrs, 0 };
    damage(g.cursor_x, g.cursor_y);
    g.cursor_x++;
}

void screen_write(Str utf8)
{
    usize i = 0;
    while (i < utf8.size()) {
        char32_t ch;
        usize len = utf8_decode(utf8, i, ch);
        if (!len)
            return; // a truncated sequence at the end
        i += len;

        if (ch == '\n')
            screen_newline();
        else
            screen_put(ch);
    }
}

void screen_newline()
{
    if (!sized())
        return;
    g.cursor_x = 0;
    next_row();
}

void screen_backspace()
{
    if (!sized() || (g.cursor_x == 0 && g.cursor_y == 0))
        return;
    if (g.cursor_x) {
        g.cursor_x--;
    } else {
        g.cursor_x = g.cols - 1;
        g.cursor_y--;
    }
    g_cells[g.cursor_y * g.cols + g.cursor_x] = blank();
    damage(g.cursor_x, g.cursor_y);
}

void screen_move(u32 x, u32 y)
{
    if (!sized())
        return;
    g.cursor_x = min(x, g.cols - 1);
    g.cursor_y = min(y, g.rows - 1);
}

void screen_cursor(bool on)
{
    g_cursor_live = on;
    if (!g_view_cells) // a view hides it, and gives it back on the way home
        g.cursor_on = on;
    if (sized())
        damage(min(g.cursor_x, g.cols - 1), g.cursor_y);
}

bool screen_cursor_on()
{
    return g_cursor_live;
}

void screen_clear()
{
    if (!sized())
        return;
    for (u32 y = 0; y < g.rows; y++)
        clear_row(g_cells, g.cols, y);
    g.cursor_x = g.cursor_y = 0;
    damage_all();
}

Rect screen_damage()
{
    if (!g_dirty)
        return Rect{ 0, 0, 0, 0 };
    return Rect{ g_x0, g_y0, g_x1 - g_x0, g_y1 - g_y0 };
}

void screen_touch(u32 x, u32 y, u32 w, u32 h)
{
    if (!sized() || !w || !h || x >= g.cols || y >= g.rows)
        return;
    u32 x1 = min(x + w, g.cols);
    u32 y1 = min(y + h, g.rows);
    damage(x, y);
    damage(x1 - 1, y1 - 1);
}

u32 screen_view()
{
    return g_view;
}

u32 screen_history()
{
    return g_hist ? g_hist_count : 0;
}

u32 screen_view_scroll(i32 delta)
{
    if (!sized())
        return 0;

    i64 want = i64(g_view) - i64(delta); // back is negative, and pages upwards
    if (want < 0)
        want = 0;
    if (want > i64(screen_history()))
        want = i64(screen_history());

    if (u32(want) == g_view)
        return g_view; // at that end already: damage nothing
    if (want == 0) {
        screen_view_home();
        return 0;
    }
    if (!view_open())
        return g_view; // nothing to compose into; stay put

    g_view = u32(want);
    view_compose();
    damage_all();
    return g_view;
}

void screen_view_home()
{
    if (!g_view_cells)
        return; // the common case: every keystroke comes through here

    heap_free(g_view_cells);
    g_view_cells = nullptr;
    g_view       = 0;
    g_view_dirty = false;
    g.cells      = u32(reinterpret_cast<usize>(g_cells));
    g.cursor_on  = g_cursor_live;
    damage_all();
}

void screen_flush()
{
    // Output has moved the rows under the view: once a tick, not once a row.
    if (g_view_dirty)
        view_compose();

    // The cursor is drawn, not stored, so moving it dirties two cells.
    if (sized() && (g.cursor_x != g_drawn_x || g.cursor_y != g_drawn_y)) {
        damage(min(g_drawn_x, g.cols - 1), min(g_drawn_y, g.rows - 1));
        damage(min(g.cursor_x, g.cols - 1), g.cursor_y);
        g_drawn_x = g.cursor_x;
        g_drawn_y = g.cursor_y;
    }
    if (!g_dirty)
        return;
    host_present(g_x0, g_y0, g_x1 - g_x0, g_y1 - g_y0);
    g_dirty = false;
}

void screen_reset()
{
    heap_free(g_view_cells);
    g_view_cells = nullptr;
    g_view       = 0;
    g_view_dirty = false;
    hist_drop();

    heap_free(g_cells);
    g_cells       = nullptr;
    g             = Screen{};
    g_fg          = COLOR_WHITE;
    g_bg          = COLOR_BLACK;
    g_attrs       = 0;
    g_dirty       = false;
    g_cursor_live = false;
    g_drawn_x = g_drawn_y = 0;
    g_scrolled            = 0;
}

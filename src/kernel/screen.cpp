#include "screen.h"

#include "alloc.h"
#include "host.h"
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

bool sized() {
    return g_cells && g.cols && g.rows;
}

Cell blank() {
    return Cell{0, g_fg, g_bg, g_attrs, 0};
}

void damage(u32 x, u32 y) {
    if (!g_dirty) {
        g_x0 = x;
        g_y0 = y;
        g_x1 = x + 1;
        g_y1 = y + 1;
        g_dirty = true;
        return;
    }
    g_x0 = min(g_x0, x);
    g_y0 = min(g_y0, y);
    g_x1 = max(g_x1, x + 1);
    g_y1 = max(g_y1, y + 1);
}

void damage_all() {
    if (!sized())
        return;
    g_x0 = 0;
    g_y0 = 0;
    g_x1 = g.cols;
    g_y1 = g.rows;
    g_dirty = true;
}

void clear_row(Cell *cells, u32 cols, u32 y) {
    Cell b = blank();
    for (u32 x = 0; x < cols; x++)
        cells[y * cols + x] = b;
}

void scroll() {
    for (u32 y = 1; y < g.rows; y++)
        __builtin_memcpy(g_cells + (y - 1) * g.cols, g_cells + y * g.cols, g.cols * sizeof(Cell));
    clear_row(g_cells, g.cols, g.rows - 1);
    if (g_drawn_y)
        g_drawn_y--;
    damage_all();
}

// Down one row, scrolling when there is no row left.
void next_row() {
    if (g.cursor_y + 1 < g.rows)
        g.cursor_y++;
    else
        scroll();
}

// The wrap is deferred: cursor_x sits at cols until the next character is
// written, so filling the last column does not scroll the screen on its own.
void wrap_pending() {
    if (g.cursor_x >= g.cols) {
        g.cursor_x = 0;
        next_row();
    }
}

} // namespace

u32 screen_resize(u32 cols, u32 rows) {
    cols = cols ? min(cols, u32(SCREEN_MAX_COLS)) : 1;
    rows = rows ? min(rows, u32(SCREEN_MAX_ROWS)) : 1;

    Cell *next = static_cast<Cell *>(heap_alloc(usize(cols) * rows * sizeof(Cell)));
    if (!next)
        return 0; // the old grid is still whole, and still the one on screen
    for (u32 y = 0; y < rows; y++)
        clear_row(next, cols, y);

    // Rows 0..cursor_y are the ones in use. Keep as many of them as fit,
    // dropping from the top, and land them at the top of the new grid — output
    // grows downwards from there, so that is where the eye already is.
    u32 live = g.rows ? min(g.cursor_y + 1, g.rows) : 0;
    u32 keep_rows = min(rows, live);
    u32 keep_cols = min(cols, g.cols);
    if (g_cells && keep_rows && keep_cols) {
        u32 src0 = live - keep_rows;
        for (u32 i = 0; i < keep_rows; i++)
            __builtin_memcpy(next + i * cols, g_cells + (src0 + i) * g.cols,
                             keep_cols * sizeof(Cell));

        g.cursor_y = g.cursor_y - src0; // cursor_y is live - 1, so keep_rows - 1
        if (g.cursor_x > cols)
            g.cursor_x = cols;
    } else {
        g.cursor_x = g.cursor_y = 0;
    }

    heap_free(g_cells);
    g_cells = next;
    g.magic = SCREEN_MAGIC;
    g.cols = cols;
    g.rows = rows;
    g.cells = u32(reinterpret_cast<usize>(next));
    g_drawn_x = g.cursor_x;
    g_drawn_y = g.cursor_y;
    damage_all();
    return u32(reinterpret_cast<usize>(&g));
}

const Screen &screen() {
    return g;
}

Cell *screen_cells() {
    return g_cells;
}

void screen_style(u8 fg, u8 bg, u8 attrs) {
    g_fg = fg;
    g_bg = bg;
    g_attrs = attrs;
}

void screen_put(char32_t ch) {
    if (!sized())
        return;
    wrap_pending();
    g_cells[g.cursor_y * g.cols + g.cursor_x] = Cell{ch, g_fg, g_bg, g_attrs, 0};
    damage(g.cursor_x, g.cursor_y);
    g.cursor_x++;
}

void screen_write(Str utf8) {
    usize i = 0;
    while (i < utf8.size()) {
        u8 c = u8(utf8[i]);
        char32_t ch;
        usize len;
        if (c < 0x80) {
            ch = c;
            len = 1;
        } else if ((c & 0xe0) == 0xc0) {
            ch = c & 0x1f;
            len = 2;
        } else if ((c & 0xf0) == 0xe0) {
            ch = c & 0x0f;
            len = 3;
        } else if ((c & 0xf8) == 0xf0) {
            ch = c & 0x07;
            len = 4;
        } else {
            i++; // a stray continuation byte
            continue;
        }
        if (i + len > utf8.size())
            return;
        for (usize k = 1; k < len; k++)
            ch = (ch << 6) | (u8(utf8[i + k]) & 0x3f);
        i += len;

        if (ch == '\n')
            screen_newline();
        else
            screen_put(ch);
    }
}

void screen_newline() {
    if (!sized())
        return;
    g.cursor_x = 0;
    next_row();
}

void screen_backspace() {
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

void screen_move(u32 x, u32 y) {
    if (!sized())
        return;
    g.cursor_x = min(x, g.cols - 1);
    g.cursor_y = min(y, g.rows - 1);
}

void screen_cursor(bool on) {
    g.cursor_on = on;
    if (sized())
        damage(min(g.cursor_x, g.cols - 1), g.cursor_y);
}

void screen_clear() {
    if (!sized())
        return;
    for (u32 y = 0; y < g.rows; y++)
        clear_row(g_cells, g.cols, y);
    g.cursor_x = g.cursor_y = 0;
    damage_all();
}

Rect screen_damage() {
    if (!g_dirty)
        return Rect{0, 0, 0, 0};
    return Rect{g_x0, g_y0, g_x1 - g_x0, g_y1 - g_y0};
}

void screen_flush() {
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

void screen_reset() {
    heap_free(g_cells);
    g_cells = nullptr;
    g = Screen{};
    g_fg = COLOR_WHITE;
    g_bg = COLOR_BLACK;
    g_attrs = 0;
    g_dirty = false;
    g_drawn_x = g_drawn_y = 0;
}

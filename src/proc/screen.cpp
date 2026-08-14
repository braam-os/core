#include "screen.h"

#include "kernel/alloc.h"

namespace {

// Both claims answer with the geometry, so taking one and sizing the grid to
// what came back is the same two lines either way.
Task<Result<void>> claim(Sys op, bool take, u32 &cols, u32 &rows)
{
    Result<SysReply> r = co_await sys_call(op, take ? 1 : 0);
    if (r.is_err())
        co_return Err(r.error());
    if (r.value().data.size() < 8)
        co_return Err(Error::Io);

    const u8 *p = reinterpret_cast<const u8 *>(r.value().data.data());
    cols        = sys_get_u32(p);
    rows        = sys_get_u32(p + 4);
    co_return {};
}

} // namespace

ProcScreen::~ProcScreen()
{
    heap_free(grid_.cells);
}

Result<void> ProcScreen::resize(u32 cols, u32 rows)
{
    if (grid_.cols == cols && grid_.rows == rows)
        return {};

    usize n     = usize(cols) * rows * sizeof(Cell);
    Cell *cells = n ? static_cast<Cell *>(heap_alloc(n)) : nullptr;
    if (n && !cells)
        return Err(Error::NoMemory);

    heap_free(grid_.cells);
    grid_.cells = cells;
    grid_.cols  = cols;
    grid_.rows  = rows;
    if (n)
        __builtin_memset(cells, 0, n);

    // Everything is new, so everything is damaged: the next flush repaints.
    grid_.damage = Rect{ 0, 0, 0, 0 };
    grid_.touch(0, 0, cols, rows);
    return {};
}

Task<Result<void>> ProcScreen::take_keys()
{
    u32 cols = 0, rows = 0;
    Task<Result<void>> t = claim(Sys::KeyClaim, true, cols, rows);
    if (!t)
        co_return Err(Error::NoMemory);
    if (Result<void> r = co_await t; r.is_err())
        co_return Err(r.error());
    keys_ = true;
    co_return resize(cols, rows);
}

Task<Result<void>> ProcScreen::take_screen()
{
    u32 cols = 0, rows = 0;
    Task<Result<void>> t = claim(Sys::ScreenEnter, true, cols, rows);
    if (!t)
        co_return Err(Error::NoMemory);
    if (Result<void> r = co_await t; r.is_err())
        co_return Err(r.error());
    screen_ = true;
    co_return resize(cols, rows);
}

Pane ProcScreen::body()
{
    Pane r = root();
    return r.height() > 1 ? r.top(r.height() - 1) : r;
}

Pane ProcScreen::status()
{
    return root().bottom(1);
}

Task<Result<void>> ProcScreen::flush()
{
    Rect d = grid_.take_damage();
    if (!d.w || !d.h)
        co_return {};

    String out;
    u8 head[SYS_BLIT_HEAD * 4];
    sys_put_u32(head, d.x);
    sys_put_u32(head + 4, d.y);
    sys_put_u32(head + 8, d.w);
    sys_put_u32(head + 12, d.h);
    sys_put_u32(head + 16, grid_.cursor_x);
    sys_put_u32(head + 20, grid_.cursor_y);
    sys_put_u32(head + 24, grid_.cursor_on ? 1 : 0);
    if (!out.append(Str(reinterpret_cast<const char *>(head), sizeof(head))))
        co_return Err(Error::NoMemory);

    // Row by row, since the damage is a rectangle of a wider grid.
    for (u32 y = 0; y < d.h; y++) {
        const Cell *row = grid_.at(d.x, d.y + y);
        if (!row ||
            !out.append(Str(reinterpret_cast<const char *>(row), usize(d.w) * sizeof(Cell))))
            co_return Err(Error::NoMemory);
    }

    Result<SysReply> r = co_await sys_call(Sys::ScreenBlit, 0, out.str());
    if (r.is_err())
        co_return Err(r.error());
    co_return {};
}

Task<Result<Key>> ProcScreen::next_key()
{
    Result<SysReply> r = co_await sys_call(Sys::KeyRead, 0);
    if (r.is_err())
        co_return Err(r.error());
    if (r.value().data.size() < 16)
        co_return Err(Error::Io);

    const u8 *p = reinterpret_cast<const u8 *>(r.value().data.data());
    Key k{ sys_get_u32(p), sys_get_u32(p + 4) };
    if (Result<void> bad = resize(sys_get_u32(p + 8), sys_get_u32(p + 12)); bad.is_err())
        co_return Err(bad.error());
    co_return k;
}

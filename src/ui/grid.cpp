#include "grid.h"

#include "kernel/traits.h"

void Grid::touch(u32 x, u32 y, u32 w, u32 h)
{
    if (!cells || !w || !h || x >= cols || y >= rows)
        return;
    u32 x1 = min(x + w, cols);
    u32 y1 = min(y + h, rows);

    if (!damage.w) {
        damage = Rect{ x, y, x1 - x, y1 - y };
        return;
    }
    u32 dx1  = max(damage.x + damage.w, x1);
    u32 dy1  = max(damage.y + damage.h, y1);
    damage.x = min(damage.x, x);
    damage.y = min(damage.y, y);
    damage.w = dx1 - damage.x;
    damage.h = dy1 - damage.y;
}

Rect Grid::take_damage()
{
    Rect out = damage;
    damage   = Rect{ 0, 0, 0, 0 };
    return out;
}

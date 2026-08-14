// A rectangle of cells and the damage done to it. This is what the layout
// layer writes through, and it is deliberately *not* the kernel's screen: the
// kernel keeps one over `screen_cells()`, and a full-screen program keeps one
// over a buffer of its own and blits the damage across with a syscall.
//
// It is the whole of what src/ui/ needs from the world, which is what lets
// Pane, TextBuf and TextView link into a process binary. Nothing here reaches
// a host import — kernel/screen.h is included for Cell and the colours, and
// those are a header with no code behind them (Concept.md §4.3).
#pragma once

#include "kernel/screen.h"
#include "kernel/types.h"

struct Grid {
    Cell *cells = nullptr;
    u32 cols = 0, rows = 0;

    // Where the real cursor goes. A Pane sets it through place_cursor; who
    // acts on it differs — the kernel's renderer reads it, and a process sends
    // it with its next blit.
    u32 cursor_x = 0, cursor_y = 0;
    bool cursor_on = false;

    // The cells written since the last time the damage was taken. `w` is 0
    // when none have been.
    Rect damage{ 0, 0, 0, 0 };

    Cell *at(u32 x, u32 y) const
    {
        return cells && x < cols && y < rows ? cells + y * cols + x : nullptr;
    }

    void touch(u32 x, u32 y, u32 w, u32 h);

    // The damage, and forgets it.
    Rect take_damage();
};

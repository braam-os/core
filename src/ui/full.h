// The alternate screen, as RAII: a full-screen program takes the grid for as
// long as this object lives, and the shell's screen comes back when it dies.
// A destructor rather than a call at the end, so that ^C — which destroys the
// frame while it is suspended — restores the screen too.
//
// There is no second grid: the cells are copied to a heap block and copied
// back, which is what "alternate screen" means when the terminal is an array
// rather than a byte stream.
#pragma once

#include "kernel/screen.h"
#include "pane.h"

struct FullScreen {
    FullScreen();

    FullScreen(const FullScreen &)            = delete;
    FullScreen &operator=(const FullScreen &) = delete;

    ~FullScreen();

    // False when the snapshot would not allocate. The program should give up:
    // taking the screen without being able to give it back is worse than not
    // running at all.
    bool ok() const { return saved_ != nullptr; }

    // The whole grid, minus the bottom row, and that row.
    Pane body() const;

    Pane status() const;

private:
    Cell *saved_ = nullptr;
    u32 cols_ = 0, rows_ = 0;
    u32 cursor_x_ = 0, cursor_y_ = 0;
    bool cursor_on_ = false;
};

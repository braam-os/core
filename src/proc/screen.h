// The terminal, from inside a process: a grid of its own, and the syscalls
// that claim the real one and blit onto it (Concept.md §4.3).
//
// A translation unit of its own so --gc-sections keeps it out of the binaries
// that never paint, which is most of them.
//
// The two claims are held by the kernel on the process's record, and nothing
// here gives them back: a destructor cannot, since releasing one is a syscall
// and there is nothing to await with. ~Proc is what restores the screen and
// the keyboard, which it has to be anyway — a killed program runs no
// destructor of its own.
#pragma once

#include "kernel/key.h"
#include "proc/io.h"
#include "ui/pane.h"

struct ProcScreen {
    ProcScreen() = default;

    ProcScreen(const ProcScreen &)            = delete;
    ProcScreen &operator=(const ProcScreen &) = delete;

    ~ProcScreen();

    // Claims the keyboard. Do this before reading stdin, so that what is typed
    // while a slow pipe fills is queued rather than echoed at the shell.
    Task<Result<void>> take_keys();

    // Takes the alternate screen, and sizes the grid to it.
    Task<Result<void>> take_screen();

    Grid &grid() { return grid_; }

    Pane root() { return Pane::of(grid_); }

    // The whole grid minus the bottom row, and that row.
    Pane body();

    Pane status();

    // Sends the cells that changed, and the cursor with them — one syscall per
    // frame rather than three.
    Task<Result<void>> flush();

    // The next key. A resize arrives with it: the geometry rides on every
    // reply, and the grid is resized and marked whole when it changes.
    Task<Result<Key>> next_key();

private:
    Result<void> resize(u32 cols, u32 rows);

    Grid grid_;
    bool keys_   = false;
    bool screen_ = false;
};

// The line discipline, in userland, as a coroutine (Concept.md §3.5): history,
// cursor movement and kill, on top of the cell grid. There is no termios state
// machine and no escape sequence anywhere in it.
#pragma once

#include "kernel/result.h"
#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/task.h"
#include "kernel/types.h"
#include "kernel/vec.h"

enum class LineEnd : u8 {
    Enter,     // committed with Return
    Interrupt, // abandoned with ^C
};

struct Line {
    String text;
    LineEnd how = LineEnd::Enter;
};

struct LineEditor {
    // The oldest entries are dropped past this.
    static constexpr usize HISTORY_MAX = 32;

    // Draws the prompt, edits until Return or ^C, and leaves the cursor at the
    // end of the line; the caller ends the row. An error is a real failure:
    // Cancelled when the task is killed, NoMemory when the buffer will not grow.
    Task<Result<Line>> read_line(Str prompt);

    usize history() const { return history_.size(); }

private:
    void redraw();
    void place_cursor();
    bool set_text(Str utf8);
    bool set_text(const Vec<char32_t> &from);
    bool set_pending();
    bool text_of(const Vec<char32_t> &from, String &out) const;
    bool remember(Str s);
    usize word_start() const;

    Vec<char32_t> buf_;     // the line, one codepoint per cell
    Vec<char32_t> pending_; // the line being typed, parked by an Up
    Vec<String> history_;   // oldest first
    usize cur_     = 0;     // cursor index into buf_
    usize hist_    = 0;     // history_.size() means "the line being typed"
    usize painted_ = 0;     // cells the last redraw covered, so the tail erases
    u32 x0_ = 0, y0_ = 0;   // where buf_[0] draws
};

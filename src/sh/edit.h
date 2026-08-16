// The line discipline, in userland and now inside a *program* (Concept.md
// §3.5): history, cursor movement and kill-word, over the one operation a
// prompt needs — Echo, which paints a run per colour, wraps and scrolls exactly
// as Write does, and says where that left off.
//
// There is no termios state machine and no escape sequence anywhere in it, and
// there was none when this was kernel code either. What changed is that every
// line of it is now a round trip.
#pragma once

#include "kernel/string.h"
#include "kernel/vec.h"
#include "proc/io.h"

enum class LineEnd : u8 {
    Enter,     // committed with Return
    Interrupt, // abandoned with ^C
};

struct Line {
    String text;
    LineEnd how = LineEnd::Enter;
};

// The prompt in two pieces, because it is two colours and a colour is a syscall
// rather than an escape in the bytes (Concept.md §2.3). Both are the caller's
// storage and must outlive the read_line that draws them.
struct Prompt {
    Str status; // the last command's failure, in red; empty when it succeeded
    Str dir;    // the working directory's basename, white on blue; empty when unknown
    Str text;   // the prompt proper, in bright white
};

struct LineEditor {
    // The oldest entries are dropped past this.
    static constexpr usize HISTORY_MAX = 32;

    // Draws the prompt, edits until Return or ^C, and leaves the cursor at the
    // end of the line; the caller ends the row.
    //
    // The keyboard must be claimed already. The shell holds it for as long as
    // it is at a prompt and gives it back around anything it runs, so that a
    // program that wants raw keys can claim them in its turn — and so that ^C,
    // with nothing in front, arrives here as an ordinary key.
    Task<Result<Line>> read_line(Prompt prompt);

    usize history() const { return history_.size(); }

private:
    Task<Result<void>> redraw();
    Task<Result<void>> anchor(Prompt prompt);
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
    u32 cols_ = 80;         // the last geometry the kernel reported
    u32 rows_ = 24;
};

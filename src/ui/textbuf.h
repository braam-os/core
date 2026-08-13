// A buffer of logical lines, for the pager and the editor. It knows nothing
// about the screen and nothing about the filesystem: the program does its own
// I/O and hands the bytes over, which is what keeps braam_ui from depending on
// braam_fs (CLAUDE.md's layering rule).
//
// A line holds UTF-8 and the cursor is a byte offset, stepped by whole
// codepoints — the editor never has to hold a decoded copy the way LineEditor
// does, because it paints from the buffer rather than from a scratch vector.
#pragma once

#include "kernel/result.h"
#include "kernel/str.h"
#include "kernel/string.h"
#include "kernel/types.h"
#include "kernel/vec.h"

struct TextBuf {
    // Splits on '\n'. A trailing newline does not make an extra empty line; an
    // empty input is one empty line, since a file always has somewhere to type.
    Result<void> load(Str utf8);

    // One more line, for a reader that arrives a line at a time — the pager,
    // which cannot hold the whole input twice.
    Result<void> add(Str line);

    // Every line, newline-terminated. What the editor writes back out.
    Result<void> serialize(String &out) const;

    usize lines() const { return lines_.size() ? lines_.size() : 1; }

    Str line(usize i) const;

    bool modified() const { return modified_; }

    void clear_modified() { modified_ = false; }

    // All of these clamp: an out-of-range line or offset does nothing.
    Result<void> insert(usize row, usize at, Str utf8);

    // Removes the codepoint starting at `at`. Returns its length in bytes.
    usize erase(usize row, usize at);

    // Splits `row` at `at`, so the tail becomes row + 1.
    Result<void> split(usize row, usize at);

    // Appends row + 1 to `row`. Returns where the join happened.
    Result<usize> join(usize row);

    // The byte offsets of the previous and next codepoint boundary in `row`.
    usize prev(usize row, usize at) const;

    usize next(usize row, usize at) const;

    // Columns to the left of `at` — codepoints, not bytes, which is what the
    // status line and the horizontal scroll both want.
    usize column(usize row, usize at) const;

    // The inverse: the byte offset `col` codepoints into the row.
    usize offset(usize row, usize col) const;

private:
    Result<void> ensure(usize row);

    Vec<String> lines_;
    bool modified_ = false;
};

#include "edit.h"

#include "kernel/key.h"
#include "kernel/screen.h"
#include "kernel/text.h"
#include "kernel/traits.h"

namespace {

bool is_word(char32_t c)
{
    return c != ' ' && c != '\t';
}

} // namespace

void LineEditor::place_cursor()
{
    const Screen &s = screen();
    if (!s.cols)
        return;

    // At an exact multiple of cols the wrap column is unreachable, and column
    // 0 of the next row is where the next character actually lands — which the
    // division already gives.
    usize off = x0_ + cur_;
    screen_move(u32(off % s.cols), y0_ + u32(off / s.cols));
}

// One unconditional repaint from the anchor. There is no erase-to-end-of-line,
// so the tail of a shortened line is blanked by hand.
void LineEditor::redraw()
{
    const Screen &s = screen();
    if (!s.cols)
        return;
    if (x0_ >= s.cols)
        x0_ = 0;

    screen_move(x0_, y0_);
    for (usize i = 0; i < buf_.size(); i++)
        screen_put(buf_[i]);
    for (usize i = buf_.size(); i < painted_; i++)
        screen_put(' ');

    usize total = buf_.size() > painted_ ? buf_.size() : painted_;
    painted_    = buf_.size();

    // Nothing counts scrolls, so infer them: the writes above should have
    // ended on this row, and a shortfall is exactly how far the grid moved.
    if (total) {
        u32 want = y0_ + u32((x0_ + total - 1) / s.cols);
        if (s.cursor_y < want)
            y0_ = want - s.cursor_y < y0_ ? y0_ - (want - s.cursor_y) : 0;
    }

    place_cursor();
}

bool LineEditor::set_text(Str utf8)
{
    buf_.clear();
    usize i = 0;
    char32_t ch;
    while (usize n = utf8_decode(utf8, i, ch)) {
        if (!buf_.push(ch))
            return false;
        i += n;
    }
    cur_ = buf_.size();
    return true;
}

bool LineEditor::set_text(const Vec<char32_t> &from)
{
    buf_.clear();
    if (!buf_.reserve(from.size()))
        return false;
    for (usize i = 0; i < from.size(); i++)
        buf_.push(from[i]);
    cur_ = buf_.size();
    return true;
}

bool LineEditor::text_of(const Vec<char32_t> &from, String &out) const
{
    out.clear();
    char b[4];
    for (usize i = 0; i < from.size(); i++)
        if (!out.append(Str(b, utf8_encode(from[i], b))))
            return false;
    return true;
}

bool LineEditor::remember(Str s)
{
    if (s.empty() || (!history_.empty() && history_.back() == s))
        return true;

    String copy;
    if (!copy.assign(s) || !history_.push(move(copy)))
        return false;
    if (history_.size() > HISTORY_MAX)
        history_.erase(0);
    return true;
}

// Back over any spaces, then back over the word before them.
usize LineEditor::word_start() const
{
    usize i = cur_;
    while (i && !is_word(buf_[i - 1]))
        i--;
    while (i && is_word(buf_[i - 1]))
        i--;
    return i;
}

Task<Result<Line>> LineEditor::read_line(Str prompt)
{
    buf_.clear();
    pending_.clear();
    cur_     = 0;
    painted_ = 0;
    hist_    = history_.size();

    // The prompt goes out first and the anchor is where it ends — on a column
    // screen_move can reach, which the deferred wrap column is not.
    if (screen().cursor_x != 0)
        screen_newline();
    screen_write(prompt);
    if (screen().cursor_x >= screen().cols)
        screen_newline();
    x0_ = screen().cursor_x;
    y0_ = screen().cursor_y;
    place_cursor();

    for (;;) {
        Result<Key> r = co_await keys().recv();
        if (r.is_err()) {
            // A stray wake resumes the receiver with nothing to take. That is
            // not end of input, and the line is still being edited.
            if (r.error() == Error::Again)
                continue;
            co_return Err(r.error());
        }

        Key k     = r.value();
        bool ctrl = (k.mods & MOD_CTRL) != 0;
        bool alt  = (k.mods & MOD_ALT) != 0;

        if (k.printable()) {
            if (!buf_.insert(cur_, k.code))
                co_return Err(Error::NoMemory);
            cur_++;
        } else if (k.code == KEY_ENTER) {
            cur_ = buf_.size();
            redraw();
            Line line;
            if (!text_of(buf_, line.text) || !remember(line.text.str()))
                co_return Err(Error::NoMemory);
            co_return move(line);
        } else if (ctrl && k.code == 'c') {
            cur_ = buf_.size();
            redraw();
            screen_write("^C");
            Line line;
            line.how = LineEnd::Interrupt;
            co_return move(line);
        } else if (k.code == KEY_BACKSPACE || (ctrl && k.code == 'h')) {
            if (alt) {
                usize at = word_start();
                buf_.erase(at, cur_ - at);
                cur_ = at;
            } else if (cur_) {
                buf_.erase(cur_ - 1);
                cur_--;
            }
        } else if (k.code == KEY_DELETE || (ctrl && k.code == 'd')) {
            if (cur_ < buf_.size())
                buf_.erase(cur_);
        } else if (k.code == KEY_LEFT || (ctrl && k.code == 'b')) {
            if (cur_)
                cur_--;
        } else if (k.code == KEY_RIGHT || (ctrl && k.code == 'f')) {
            if (cur_ < buf_.size())
                cur_++;
        } else if (k.code == KEY_HOME || (ctrl && k.code == 'a')) {
            cur_ = 0;
        } else if (k.code == KEY_END || (ctrl && k.code == 'e')) {
            cur_ = buf_.size();
        } else if (ctrl && k.code == 'k') {
            buf_.erase(cur_, buf_.size() - cur_);
        } else if (ctrl && k.code == 'u') {
            buf_.erase(0, cur_);
            cur_ = 0;
        } else if (ctrl && k.code == 'w') {
            usize at = word_start();
            buf_.erase(at, cur_ - at);
            cur_ = at;
        } else if (ctrl && k.code == 'l') {
            screen_clear();
            screen_write(prompt);
            x0_      = screen().cursor_x;
            y0_      = screen().cursor_y;
            painted_ = 0;
        } else if (k.code == KEY_UP) {
            if (!hist_)
                continue;
            // The line being typed is parked on the way out, and comes back
            // when Down walks past the newest entry.
            if (hist_ == history_.size() && !set_pending())
                co_return Err(Error::NoMemory);
            hist_--;
            if (!set_text(history_[hist_].str()))
                co_return Err(Error::NoMemory);
        } else if (k.code == KEY_DOWN) {
            if (hist_ == history_.size())
                continue;
            hist_++;
            bool ok =
                hist_ == history_.size() ? set_text(pending_) : set_text(history_[hist_].str());
            if (!ok)
                co_return Err(Error::NoMemory);
        } else {
            continue; // an unbound key changed nothing
        }

        redraw();
    }
}

bool LineEditor::set_pending()
{
    pending_.clear();
    if (!pending_.reserve(buf_.size()))
        return false;
    for (usize i = 0; i < buf_.size(); i++)
        pending_.push(buf_[i]);
    return true;
}

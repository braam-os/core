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

// One unconditional repaint from the anchor. There is no erase-to-end-of-line,
// so the tail of a shortened line is blanked by hand — and the whole thing is
// one syscall, because a keystroke pays a round trip for each (§4.3).
Task<Result<void>> LineEditor::redraw()
{
    if (!cols_)
        co_return {};
    if (x0_ >= cols_)
        x0_ = 0;

    String out;
    char b[4];
    for (usize i = 0; i < buf_.size(); i++)
        if (!out.append(Str(b, utf8_encode(buf_[i], b))))
            co_return Err(Error::NoMemory);
    for (usize i = buf_.size(); i < painted_; i++)
        if (!out.push(' '))
            co_return Err(Error::NoMemory);

    painted_ = buf_.size();

    // Anchor, bytes and cursor in one operation, so one tick and one present:
    // nothing hides the cursor because it is never seen walking the line, and
    // nothing asks where the write ended because `scrolled` says. One run, in
    // whatever colour the prompt left sticky.
    const StyledRun runs[]  = { { SYS_STYLE_KEEP, out.str() } };
    Task<Result<Painted>> t = cursor_echo(x0_, y0_, cur_, SYS_ECHO_SHOW, runs);
    if (!t)
        co_return Err(Error::NoMemory);
    Result<Painted> r = co_await t;
    if (r.is_err())
        co_return Err(r.error());

    cols_ = r.value().cursor.at.cols;
    rows_ = r.value().cursor.at.rows;
    y0_   = r.value().scrolled < y0_ ? y0_ - r.value().scrolled : 0;
    co_return {};
}

// The prompt, the anchor and the cursor in one operation: a run per colour, a
// last run with no bytes to put the default back under what is typed, and the
// two flags that ask for a fresh row and for the cursor where the write ended.
// An empty status or directory is an empty run rather than a skipped one, so
// the count is always four.
Task<Result<void>> LineEditor::anchor(Prompt prompt)
{
    const StyledRun runs[] = {
        { sys_style_pack(COLOR_RED, COLOR_BLACK, 0), prompt.status },
        { sys_style_pack(COLOR_WHITE, COLOR_BLUE, 0), prompt.dir },
        { sys_style_pack(COLOR_WHITE | COLOR_BRIGHT, COLOR_BLACK, 0), prompt.text },
        { sys_style_pack(COLOR_WHITE, COLOR_BLACK, 0), Str() },
    };

    Task<Result<Painted>> t =
        cursor_echo(0, 0, 0, SYS_ECHO_FRESH | SYS_ECHO_END | SYS_ECHO_SHOW, runs);
    if (!t)
        co_return Err(Error::NoMemory);
    Result<Painted> r = co_await t;
    if (r.is_err())
        co_return Err(r.error());

    // Straight from the reply, not through `scrolled`: under END the reply is
    // where the write ended, wrap column and resize and all.
    x0_   = r.value().cursor.x;
    y0_   = r.value().cursor.y;
    cols_ = r.value().cursor.at.cols;
    rows_ = r.value().cursor.at.rows;
    co_return {};
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

Task<Result<Line>> LineEditor::read_line(Prompt prompt)
{
    buf_.clear();
    pending_.clear();
    cur_     = 0;
    painted_ = 0;
    hist_    = history_.size();

    if (Task<Result<void>> t = anchor(prompt)) {
        if (Result<void> r = co_await t; r.is_err())
            co_return Err(r.error());
    } else
        co_return Err(Error::NoMemory);

    for (;;) {
        Task<Result<KeyPress>> kt = key_read();
        if (!kt)
            co_return Err(Error::NoMemory);
        Result<KeyPress> r = co_await kt;
        if (r.is_err())
            co_return Err(r.error());

        Key k{ r.value().code, r.value().mods };
        cols_     = r.value().at.cols;
        rows_     = r.value().at.rows;
        bool ctrl = (k.mods & MOD_CTRL) != 0;
        bool alt  = (k.mods & MOD_ALT) != 0;

        if (k.printable()) {
            if (!buf_.insert(cur_, k.code))
                co_return Err(Error::NoMemory);
            cur_++;
        } else if (k.code == KEY_ENTER) {
            cur_ = buf_.size();
            if (Task<Result<void>> t = redraw()) {
                if (Result<void> bad = co_await t; bad.is_err())
                    co_return Err(bad.error());
            }
            Line line;
            if (!text_of(buf_, line.text) || !remember(line.text.str()))
                co_return Err(Error::NoMemory);
            co_return move(line);
        } else if (ctrl && k.code == 'c') {
            cur_ = buf_.size();
            if (Task<Result<void>> t = redraw()) {
                if (Result<void> bad = co_await t; bad.is_err())
                    co_return Err(bad.error());
            }
            if (Task<Result<void>> t = write_all(SYS_STDOUT, "^C"))
                co_await t;
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
            if ((co_await sys_call(Sys::ScreenClear, 0)).is_err())
                co_return Err(Error::Io);
            painted_ = 0;
            if (Task<Result<void>> t = anchor(prompt)) {
                if (Result<void> bad = co_await t; bad.is_err())
                    co_return Err(bad.error());
            }
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

        if (Task<Result<void>> t = redraw()) {
            if (Result<void> bad = co_await t; bad.is_err())
                co_return Err(bad.error());
        } else
            co_return Err(Error::NoMemory);
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

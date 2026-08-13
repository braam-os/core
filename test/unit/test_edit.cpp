#include "harness.h"
#include "kernel/key.h"
#include "kernel/sched.h"
#include "kernel/screen.h"
#include "user/edit.h"

namespace {

// The editor under test lives in test_edit's frame: it holds Vecs, so it
// cannot be a global — a non-trivial destructor needs __cxa_atexit.
LineEditor *ed;

char got[128];
usize got_n;
LineEnd got_how;
bool done, failed;
Error got_err;

Task<i32> reader(Str prompt)
{
    Result<Line> r = co_await ed->read_line(prompt);
    if (r.is_err()) {
        failed  = true;
        got_err = r.error();
        co_return 1;
    }
    got_how = r.value().how;
    got_n   = r.value().text.size();
    if (got_n > sizeof(got))
        got_n = sizeof(got);
    __builtin_memcpy(got, r.value().text.data(), got_n);
    done = true;
    co_return 0;
}

Str text()
{
    return Str(got, got_n);
}

u32 pid;

// Starts a fresh read_line and runs it up to its first suspension.
void start(Str prompt)
{
    done = failed = false;
    got_n         = 0;
    sched_reset();
    keys().clear();
    pid = sched_spawn(reader(prompt));
    CHECK(pid != 0);
    sched_tick(0);
}

void press(u32 code, u32 mods = 0)
{
    keys().try_send(Key{ code, mods });
    sched_tick(0);
}

void type(Str s)
{
    for (usize i = 0; i < s.size(); i++)
        press(u32(u8(s[i])));
}

// The text of one row, with trailing blanks trimmed.
Str row(u32 y)
{
    static char buf[SCREEN_MAX_COLS];
    const Cell *cells = screen_cells();
    usize n           = 0;
    for (u32 x = 0; x < screen().cols && n < sizeof(buf); x++) {
        char32_t ch = cells[y * screen().cols + x].ch;
        buf[n++]    = ch && ch < 0x80 ? char(ch) : ' ';
    }
    while (n && buf[n - 1] == ' ')
        n--;
    return Str(buf, n);
}

} // namespace

void test_edit()
{
    test_begin("edit");

    LineEditor editor;
    ed = &editor;

    screen_reset();
    CHECK(screen_resize(20, 4));

    // Typing lands in cells after the prompt, and the cursor tracks it.
    start("$ ");
    type("abc");
    CHECK(row(0) == "$ abc");
    CHECK_EQ(screen().cursor_x, 5);
    CHECK_EQ(screen().cursor_y, 0);
    CHECK(!done);

    // Left, then insert in the middle.
    press(KEY_LEFT);
    CHECK_EQ(screen().cursor_x, 4);
    type("X");
    CHECK(row(0) == "$ abXc");
    CHECK_EQ(screen().cursor_x, 5);

    // Home and End, by both spellings.
    press(KEY_HOME);
    CHECK_EQ(screen().cursor_x, 2);
    press(KEY_END);
    CHECK_EQ(screen().cursor_x, 6);
    press('a', MOD_CTRL);
    CHECK_EQ(screen().cursor_x, 2);
    press('e', MOD_CTRL);
    CHECK_EQ(screen().cursor_x, 6);

    // Delete forward, then backspace.
    press(KEY_HOME);
    press(KEY_DELETE);
    CHECK(row(0) == "$ bXc"); // the tail was blanked, not left behind
    press(KEY_END);
    press(KEY_BACKSPACE);
    CHECK(row(0) == "$ bX");

    // Kill to end, kill to start, and kill-word. row() trims trailing blanks,
    // so a killed word's separator shows up in the cursor rather than the text.
    press(KEY_ENTER);
    CHECK(done);
    CHECK(text() == "bX");

    screen_clear();
    start("$ ");
    type("one two three");
    press('w', MOD_CTRL);
    CHECK(row(0) == "$ one two");
    CHECK_EQ(screen().cursor_x, 10); // "one two " — the space survives
    press(KEY_BACKSPACE, MOD_ALT);   // the binding the browser does not eat
    CHECK(row(0) == "$ one");
    CHECK_EQ(screen().cursor_x, 6);
    press('u', MOD_CTRL);
    CHECK(row(0) == "$");
    CHECK_EQ(screen().cursor_x, 2);
    type("keep");
    CHECK(row(0) == "$ keep");
    press(KEY_HOME);
    press('k', MOD_CTRL);
    CHECK(row(0) == "$");
    press('c', MOD_CTRL);
    CHECK(done);
    CHECK(got_how == LineEnd::Interrupt);
    CHECK(text().empty());
    CHECK(row(0) == "$ ^C");

    // A cursor at an exact multiple of cols sits at column 0 of the next row:
    // the deferred-wrap column is unreachable by screen_move.
    screen_clear();
    start("$ ");
    type("012345678901234567"); // 18 chars after a 2-column prompt
    CHECK_EQ(screen().cursor_x, 0);
    CHECK_EQ(screen().cursor_y, 1);
    press(KEY_LEFT);
    CHECK_EQ(screen().cursor_x, 19);
    CHECK_EQ(screen().cursor_y, 0);

    // Backspace across the wrap boundary.
    press(KEY_END);
    press(KEY_BACKSPACE);
    CHECK(row(0) == "$ 01234567890123456");
    CHECK_EQ(screen().cursor_x, 19);

    // A line long enough to scroll moves the anchor with it. The prompt
    // starts on row 2, and 45 characters push the grid up by one row.
    screen_clear();
    screen_write("a\nb\n");
    start("$ ");
    for (u32 i = 0; i < 45; i++)
        press('x');
    CHECK(row(1) == "$ xxxxxxxxxxxxxxxxxx");
    press(KEY_HOME);
    CHECK_EQ(screen().cursor_x, 2);
    CHECK_EQ(screen().cursor_y, 1); // the anchor followed the scroll
    press('c', MOD_CTRL);

    // History: Up recalls, Down walks back, and the line being typed survives.
    // A fresh editor, so the lines committed above are not in the way.
    LineEditor recall;
    ed = &recall;

    screen_clear();
    start("$ ");
    type("one");
    press(KEY_ENTER);
    CHECK(text() == "one");

    start("$ ");
    type("two");
    press(KEY_ENTER);
    CHECK(text() == "two");

    start("$ ");
    type("ab");
    press(KEY_UP);
    CHECK(row(2) == "$ two");
    press(KEY_UP);
    CHECK(row(2) == "$ one");
    press(KEY_UP); // already at the oldest
    CHECK(row(2) == "$ one");
    press(KEY_DOWN);
    CHECK(row(2) == "$ two");
    CHECK_EQ(ed->history(), 2);
    press(KEY_DOWN);
    CHECK(row(2) == "$ ab"); // the parked line came back
    press(KEY_DOWN);
    CHECK(row(2) == "$ ab");
    press(KEY_ENTER);
    CHECK(text() == "ab");
    CHECK_EQ(ed->history(), 3);

    // An empty line and an immediate duplicate are not remembered.
    start("$ ");
    press(KEY_ENTER);
    CHECK(text().empty());
    start("$ ");
    type("ab");
    press(KEY_ENTER);
    CHECK_EQ(ed->history(), 3);

    // A stray wake resumes the receiver with an empty ring. The editor must
    // treat Error::Again as a retry, not as end of input.
    screen_clear();
    start("$ ");
    sched_wake(1, 0, 0); // the receiver's token: the first one after a reset
    sched_tick(0);
    CHECK(!done);
    CHECK(!failed);
    type("hi!");
    press(KEY_ENTER);
    CHECK(done);
    CHECK(text() == "hi!");

    // Cancelling a task parked on the keyboard unwinds it (Concept.md §8.1).
    start("$ ");
    type("half");
    sched_cancel(pid);
    sched_tick(0);
    CHECK(!done);
    CHECK(failed);
    CHECK(got_err == Error::Cancelled);
    CHECK(!sched_alive(pid));

    sched_reset();
    screen_reset();
}

#include "harness.h"

#include "kernel/sched.h"
#include "kernel/screen.h"
#include "user/prog.h"
#include "user/tty.h"

namespace {

// Every program in src/prog/. The count is the tripwire for a registrar the
// linker dropped: those fail silently, and only a count notices.
constexpr Str NAMES[] = {"clear", "echo", "false", "help", "sleep", "true", "version"};

i32 status;

// Awaiting the child is the only way an exit code is observable: sched_spawn
// reaps a finished job and discards its i32. This is what the shell does.
Task<i32> runner(const Program *p, Args args, Stdio io) {
    status = co_await p->run(args, io);
    co_return 0;
}

bool before(Str a, Str b) {
    usize n = a.size() < b.size() ? a.size() : b.size();
    for (usize i = 0; i < n; i++)
        if (a[i] != b[i])
            return u8(a[i]) < u8(b[i]);
    return a.size() < b.size();
}

// Runs to completion, ticking the clock forward far enough for a sleep.
i32 run(Str name, Span<const Str> argv, Stdio io) {
    const Program *p = program_find(name);
    if (!p) {
        CHECK(p != nullptr);
        return -1;
    }
    status = -1;
    sched_reset();
    CHECK(sched_spawn(runner(p, Args{argv}, io)) != 0);
    sched_tick(0);
    sched_tick(1000);
    return status;
}

// The text of one row, with trailing blanks trimmed.
Str row(u32 y, char *out, usize cap) {
    const Cell *cells = screen_cells();
    usize n = 0;
    for (u32 x = 0; x < screen().cols && n < cap; x++) {
        char32_t ch = cells[y * screen().cols + x].ch;
        out[n++] = ch && ch < 0x80 ? char(ch) : ' ';
    }
    while (n && out[n - 1] == ' ')
        n--;
    return Str(out, n);
}

} // namespace

void test_prog() {
    test_begin("prog");

    // The registry holds exactly the expected set, in name order, filled in.
    usize n = 0;
    Str last;
    for (const Program *p = program_first(); p; p = p->next) {
        CHECK(!p->name.empty());
        CHECK(!p->usage.empty());
        CHECK(p->run != nullptr);
        if (n)
            CHECK(before(last, p->name));
        if (n < sizeof(NAMES) / sizeof(NAMES[0]))
            CHECK(p->name == NAMES[n]);
        last = p->name;
        n++;
    }
    CHECK_EQ(n, sizeof(NAMES) / sizeof(NAMES[0]));

    CHECK(program_find("true") != nullptr);
    CHECK(program_find("nope") == nullptr);
    CHECK(program_find("") == nullptr);

    screen_reset();
    CHECK(screen_resize(80, 16)); // wide enough that no usage line wraps
    Stdio io = stdio_console();
    char buf[64];

    // Exit codes, which is the whole point of awaiting the child.
    Str argv0[] = {"true"};
    CHECK_EQ(run("true", argv0, io), 0);
    Str argv1[] = {"false"};
    CHECK_EQ(run("false", argv1, io), 1);

    // echo, with and without -n.
    screen_clear();
    Str argv2[] = {"echo", "hello", "world"};
    CHECK_EQ(run("echo", argv2, io), 0);
    CHECK(row(0, buf, sizeof(buf)) == "hello world");
    CHECK_EQ(screen().cursor_y, 1);

    screen_clear();
    Str argv3[] = {"echo", "-n", "bare"};
    CHECK_EQ(run("echo", argv3, io), 0);
    CHECK(row(0, buf, sizeof(buf)) == "bare");
    CHECK_EQ(screen().cursor_y, 0);

    // help lists every registered program, one per row, in registry order.
    screen_clear();
    Str argv4[] = {"help"};
    CHECK_EQ(run("help", argv4, io), 0);
    for (usize i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
        CHECK(row(u32(i), buf, sizeof(buf)).starts_with("  ") &&
              row(u32(i), buf, sizeof(buf)).substr(2).starts_with(NAMES[i]));

    // sleep parks on the timer queue and only then returns.
    screen_clear();
    Str argv5[] = {"sleep", "30"};
    status = -1;
    sched_reset();
    CHECK(sched_spawn(runner(program_find("sleep"), Args{argv5}, io)) != 0);
    CHECK_EQ(sched_tick(0), 30);
    CHECK_EQ(status, -1);
    CHECK_EQ(sched_tick(30), -1);
    CHECK_EQ(status, 0);

    Str argv6[] = {"sleep"};
    CHECK_EQ(run("sleep", argv6, io), 2);
    Str argv7[] = {"sleep", "later"};
    CHECK_EQ(run("sleep", argv7, io), 2);

    // clear blanks the grid and homes the cursor.
    Str argv8[] = {"clear"};
    CHECK_EQ(run("clear", argv8, io), 0);
    CHECK_EQ(screen().cursor_x, 0);
    CHECK_EQ(screen().cursor_y, 0);
    CHECK(row(0, buf, sizeof(buf)).empty());

    Str argv9[] = {"version"};
    CHECK_EQ(run("version", argv9, io), 0);
    CHECK(row(0, buf, sizeof(buf)).starts_with("braam "));

    sched_reset();
    screen_reset();
}

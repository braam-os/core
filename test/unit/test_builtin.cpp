#include "harness.h"
#include "kernel/sched.h"
#include "kernel/screen.h"
#include "user/builtin.h"
#include "user/prog.h"
#include "user/shell.h"
#include "user/tty.h"

namespace {

// The whole table. It cannot lose an entry to the linker — the array is
// referenced by name, which a self-registering list would not be — so the check
// here is that the names are the intended six and that they are in the order
// `help` prints.
constexpr Str NAMES[] = { "cd", "exit", "fg", "help", "jobs", "kill" };

i32 status;

Task<i32> runner(const Builtin *b, Args args, Stdio io)
{
    status = co_await b->run(args, io);
    co_return 0;
}

i32 run(Str name, Span<const Str> argv, Stdio io)
{
    const Builtin *b = builtin_find(name);
    if (!b) {
        CHECK(b != nullptr);
        return -1;
    }
    status = -1;
    sched_reset();
    CHECK(sched_spawn(runner(b, Args{ argv }, io)) != 0);
    sched_tick(0);
    sched_tick(1000);
    return status;
}

Str row(u32 y, char *out, usize cap)
{
    const Cell *cells = screen_cells();
    usize n           = 0;
    for (u32 x = 0; x < screen().cols && n < cap; x++) {
        char32_t ch = cells[y * screen().cols + x].ch;
        out[n++]    = ch && ch < 0x80 ? char(ch) : ' ';
    }
    while (n && out[n - 1] == ' ')
        n--;
    return Str(out, n);
}

bool before(Str a, Str b)
{
    usize n = a.size() < b.size() ? a.size() : b.size();
    for (usize i = 0; i < n; i++)
        if (a[i] != b[i])
            return u8(a[i]) < u8(b[i]);
    return a.size() < b.size();
}

} // namespace

void test_builtin()
{
    test_begin("builtin");

    Span<const Builtin> table = builtins();
    CHECK_EQ(table.size(), sizeof(NAMES) / sizeof(NAMES[0]));
    for (usize i = 0; i < table.size(); i++) {
        CHECK(table[i].name == NAMES[i]);
        CHECK(!table[i].usage.empty());
        CHECK(table[i].run != nullptr);
        if (i)
            CHECK(before(table[i - 1].name, table[i].name));
    }

    CHECK(builtin_find("cd") != nullptr);
    CHECK(builtin_find("echo") == nullptr); // a program, not a builtin
    CHECK(builtin_find("nope") == nullptr);
    CHECK(builtin_find("") == nullptr);

    screen_reset();
    CHECK(screen_resize(80, 40)); // tall enough for help, wide enough not to wrap
    Stdio io = stdio_console();
    char buf[80];

    // help lists the builtins first, in table order. /bin needs a bundle and
    // there is none here, so nothing follows them.
    screen_clear();
    Str argv0[] = { "help" };
    CHECK_EQ(run("help", argv0, io), 0);
    for (usize i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
        CHECK(row(u32(i), buf, sizeof(buf)).starts_with("  ") &&
              row(u32(i), buf, sizeof(buf)).substr(2).starts_with(NAMES[i]));

    // Usage errors are the shell's convention: 2, and a line on stderr.
    Str argv1[] = { "help", "me" };
    CHECK_EQ(run("help", argv1, io), 2);
    Str argv2[] = { "cd", "a", "b" };
    CHECK_EQ(run("cd", argv2, io), 2);
    Str argv3[] = { "jobs", "x" };
    CHECK_EQ(run("jobs", argv3, io), 2);

    // kill and fg both refuse a name that is not a job.
    Str argv4[] = { "kill", "nonsense" };
    CHECK_EQ(run("kill", argv4, io), 1);
    Str argv5[] = { "kill" };
    CHECK_EQ(run("kill", argv5, io), 2);
    Str argv6[] = { "fg", "%99" };
    CHECK_EQ(run("fg", argv6, io), 1);

    // exit asks rather than acts: the status is its own, and the request is
    // left for the shell to read at the next prompt.
    i32 wanted = -1;
    CHECK(!shell_exit_wanted(wanted));
    Str argv7[] = { "exit", "3" };
    CHECK_EQ(run("exit", argv7, io), 3);
    CHECK(shell_exit_wanted(wanted));
    CHECK_EQ(wanted, 3);
    CHECK(!shell_exit_wanted(wanted)); // read once

    Str argv8[] = { "exit", "later" };
    CHECK_EQ(run("exit", argv8, io), 2);
    CHECK(!shell_exit_wanted(wanted)); // a refused exit asks for nothing

    sched_reset();
    screen_reset();
}

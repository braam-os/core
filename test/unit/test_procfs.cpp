#include "fs/vfs.h"
#include "harness.h"
#include "kernel/fmt.h"
#include "kernel/sched.h"
#include "kernel/sysabi.h"
#include "user/procfs.h"

namespace {

// Reads a whole file through the VFS, the way `cat` does.
String slurp(Str path)
{
    String out;
    Task<Result<i32>> t = vfs_open(path, O_READ);
    if (!t)
        return out;
    Result<i32> fd = run_now(move(t));
    if (fd.is_err())
        return out;

    u64 at = 0;
    for (;;) {
        u8 block[128];
        Result<usize> r = vfs_read(fd.value(), at, block, sizeof block);
        if (r.is_err() || r.value() == 0)
            break;
        out.append(Str(reinterpret_cast<const char *>(block), r.value()));
        at += r.value();
    }
    vfs_close(fd.value());
    return out;
}

Task<i32> parked()
{
    co_await sleep_ms(10000);
    co_return 0;
}

} // namespace

void test_procfs()
{
    test_begin("procfs");

    vfs_reset();
    sched_reset();
    Fs *proc = procfs_create();
    CHECK(proc != nullptr);
    CHECK(vfs_mount("/proc", proc).is_ok());

    // The static files are there, and each says something.
    Result<Vec<Entry>> ls = run_now(vfs_list("/proc"));
    CHECK(ls.is_ok());
    CHECK(ls.value().size() >= 5);

    CHECK(!slurp("/proc/version").empty());
    CHECK(slurp("/proc/meminfo").str().starts_with("reserved "));
    CHECK(slurp("/proc/mounts").str().starts_with("/proc procfs ro"));
    CHECK(!slurp("/proc/uptime").empty());
    // The four fields the kernel answers for itself. The host's half is boot's,
    // which has not run here, so /proc/host is the kernel's alone in this case.
    CHECK(slurp("/proc/host").str().starts_with("system   braam\nrelease  "));
    CHECK(slurp("/proc/host").str().contains("\nmachine  wasm32\nscreen   "));
    CHECK(slurp("/proc/jobs").empty()); // no background jobs in this case

    // stat agrees with what a read produces, which is what `ls -l` shows.
    Result<Stat> st = run_now(vfs_stat("/proc/meminfo"));
    CHECK(st.is_ok());
    CHECK_EQ(st.value().kind == NodeKind::File, true);
    CHECK_EQ(st.value().size, slurp("/proc/meminfo").size());
    CHECK(run_now(vfs_stat("/proc")).value().kind == NodeKind::Dir);

    // Generated at open, so there is no moment to report: mtime 0 throughout,
    // and nothing to move.
    CHECK_EQ(st.value().mtime, 0u);
    for (const Entry &e : ls.value())
        CHECK_EQ(e.mtime, 0u);
    CHECK(run_now(vfs_touch("/proc/meminfo")).error() == Error::Perm);

    // A live task has a file of its own, named by its pid.
    u32 pid = sched_spawn(parked(), "parked");
    CHECK(pid != 0);
    sched_tick(0);

    Buf<32> path;
    path.put("/proc/").put(pid);
    String text = slurp(path.str());
    CHECK(text.str().starts_with("pid    "));
    CHECK(text.str().contains("name   parked"));
    CHECK(text.str().contains("state  waiting"));
    // Parked on a timer, and a task with no worker answers for nothing else: no
    // parent, no descriptors, no cap and no directory of its own.
    CHECK(text.str().contains("wait   timer"));
    CHECK(text.str().contains("worker -"));
    CHECK(!text.str().contains("\ncwd    "));
    CHECK(!text.str().contains("\nfds    "));

    // /proc/tasks is the same facts but the worker, one line per task, taken in
    // one pass: pid, name, state, wait, flags, ppid, calls, fds, mem, cap, age,
    // cwd.
    String held = slurp("/proc/tasks");
    Str tasks   = held.str();
    CHECK(tasks.contains(" parked waiting timer - 0 0 0 0 0 "));
    usize fields = 1;
    for (usize i = 0; i < tasks.size() && tasks[i] != '\n'; i++)
        if (tasks[i] == ' ')
            fields++;
    CHECK_EQ(fields, usize(12));

    // An anonymous job is the kernel's own business: no file, no line and no
    // entry in the listing, though the gauges below still count it.
    u32 anon = sched_spawn(parked(), "anon", JobId::Anon);
    CHECK(anon > SYS_PID_MAX);
    sched_tick(0);
    Buf<32> hidden;
    hidden.put("/proc/").put(anon);
    CHECK(run_now(vfs_stat(hidden.str())).is_err());
    CHECK(!slurp("/proc/tasks").str().contains(" anon "));
    sched_cancel(anon);
    sched_tick(0);

    // /proc/stat is every counter the kernel keeps, one `name value` line each.
    // The parked task above is the whole of what is running, so the four wait
    // gauges partition it: one task, on a timer, nothing runnable.
    String counted = slurp("/proc/stat");
    Str stat       = counted.str();
    CHECK(stat.starts_with("now      "));
    CHECK(stat.contains("\nresumes  "));
    CHECK(stat.contains("\nframes   "));
    CHECK(stat.contains("\ntasks    1\n"));
    CHECK(stat.contains("\nready    0\n"));
    CHECK(stat.contains("\non_timer 1\n"));
    CHECK(stat.contains("\non_host  0\n"));
    CHECK(stat.contains("\non_park  0\n"));
    // Nothing a program cannot run: the in-wasm suite steps no instance.
    CHECK(stat.contains("\nsteps    0\n"));

    // No line was silently truncated, which is the whole reason the file is
    // emitted a line at a time: eight padded, a space, at most twenty digits
    // and a newline is thirty, inside the per-line Buf's thirty-two.
    usize lines = 0, longest = 0, run = 0;
    for (usize i = 0; i < stat.size(); i++) {
        if (stat[i] != '\n') {
            run++;
            continue;
        }
        lines++;
        if (run > longest)
            longest = run;
        run = 0;
    }
    CHECK_EQ(lines, usize(24));
    CHECK(longest < 31);

    // It appears in the listing too, and goes when the task does.
    ls = run_now(vfs_list("/proc"));
    CHECK(ls.is_ok());
    bool listed = false;
    for (const Entry &e : ls.value())
        if (e.name.str() == path.str().substr(6))
            listed = true;
    CHECK(listed);

    sched_cancel(pid);
    sched_tick(0);
    CHECK(run_now(vfs_stat(path.str())).is_err());

    // Nothing here is writable, and a name that is not a file is not found.
    CHECK(run_now(vfs_open("/proc/meminfo", O_WRITE)).is_err());
    CHECK(run_now(vfs_stat("/proc/nope")).is_err());
    CHECK(run_now(vfs_mkdir("/proc/x")).is_err());

    vfs_reset();
    sched_reset();
}

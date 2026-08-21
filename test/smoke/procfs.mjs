// /proc/tasks, and ps and vmstat reformatting it.
// Part of the smoke suite; test/run.mjs runs the cases in order and
// doc/Testing.md has the rules they run by.

import {
    KEY, fail, kernel, press, prompt, regrid, resize, row, rows, run, screen, submit, type,
} from "./harness.mjs";

export function check() {
    let s = screen();
    // /proc/tasks is every task in one open, so no two rows describe different
    // moments, and `ps` reformats it the way `df` reformats /proc/mounts. Both
    // are wider than this grid, so it goes wide the way it does for help.
    submit("clear", 1185);
    regrid(100, 48, "the resize before ps failed");

    // The pump is task 1 and has no process behind it: no cwd, and a dash in
    // every field the process record fills — it is a coroutine in the kernel.
    s = submit("cat /proc/tasks", 1185.1);
    const tasks = rows(s).filter((line) => line && !line.includes("$"));
    if (!tasks.some((line) => /^1 tty \w+ \S+ \S+ 0 0 0 0 0 \d+ -$/.test(line)))
        fail(`/proc/tasks has no pump without a process: ${JSON.stringify(tasks)}`);
    if (!tasks.some((line) => /^2 init \w+ \S+ \S+ 0 \d+ \d+ \d+ \d+ \d+ \/home$/.test(line)))
        fail(`/proc/tasks has no init with a process behind it: ${JSON.stringify(tasks)}`);
    for (const line of tasks) {
        if (line.split(" ").length !== 12)
            fail(`/proc/tasks is not twelve fields: ${JSON.stringify(line)}`);
        // A syscall server is an anonymous job: no pid, and no line here.
        if (Number(line.split(" ")[0]) > 999999 || line.split(" ")[1].startsWith("/bin/"))
            fail(`/proc/tasks listed an anonymous job: ${JSON.stringify(line)}`);
    }

    // The memory a process has committed is measured on the host and rides back
    // on every step: less than the cap, and not nought, since a running instance
    // has pages. Nothing else in a browser will say what a worker holds.
    const shell = tasks.find((line) => line.startsWith("2 init "));
    const [used, cap] = shell.split(" ").slice(8, 10).map(Number);
    if (!(used > 0 && used < cap))
        fail(`init committed ${used} of ${cap} bytes, expected some of it`);
    if (cap !== 256 * 65536)
        fail(`init's cap is ${cap}, expected PROC_MAX_PAGES`);

    // ps itself is one of the tasks it lists — it is a process like any other,
    // and the shell armed it as the foreground before waiting for it. A syscall
    // server is not: it is an anonymous job, out of the pid space and out of
    // /proc, so neither file names one.
    s = submit("ps", 1185.2);
    const ps = rows(s);
    if (!ps.includes("  PID PPID NAME         STAT WAIT   CALLS FDS   MEM  ELAPSED  CWD"))
        fail(`ps did not head the table: ${JSON.stringify(ps)}`);
    if (!ps.some((line) => /^ +1 +- tty +[RS] +\S+ +- +- +- +\d+:\d\d +-$/.test(line)))
        fail(`ps did not show the pump as a kernel task: ${JSON.stringify(ps)}`);
    if (!ps.some((line) =>
        /^ +2 +- init +[RS] +\S+ +\d+ +\d+ +\d+(\.\d)?[BKM] +\d+:\d\d +\/home$/.test(line)))
        fail(`ps did not scale init's memory: ${JSON.stringify(ps)}`);
    if (!ps.some((line) => /^ +\d+ +2 ps +S\+ +\S+ +\d/.test(line)))
        fail(`ps did not list itself in the foreground: ${JSON.stringify(ps)}`);
    if (ps.some((line) => /^ +\d+ +\d+ \/bin\//.test(line)))
        fail(`ps listed a syscall server: ${JSON.stringify(ps)}`);
    if (ps.some((line) => Number(line.trim().split(" ")[0]) > 999999))
        fail(`ps listed a task outside the pid space: ${JSON.stringify(ps)}`);
    s = submit("ps -x", 1185.3);
    if (!rows(s).includes("usage: ps") || !rows(s).includes(prompt(2)))
        fail(`ps -x printed ${JSON.stringify(rows(s))}, expected a usage line`);

    // vmstat is the same counters as rates. This is the only place the rate
    // arithmetic runs at all: the in-wasm suite cannot step a program.
    s = submit("clear", 1185.4);
    s = submit("vmstat", 1185.5);
    const vm = rows(s).filter((line) => line && !line.includes("$"));
    console.error("VMSTAT-DEFAULT:\n" + vm.join("\n"));
    if (!vm[0] || !/^-+procs-+\s+-+memory-+\s+-+alloc-+\s+-+faults-+\s+-*loop-*$/.test(vm[0]))
        fail(`vmstat did not rule its groups: ${JSON.stringify(vm)}`);
    if (vm[1] !== "  r  t  h  p      use    fre       al     fr  gr      in    sy    cs      tk")
        fail(`vmstat did not name its columns: ${JSON.stringify(vm[1])}`);
    // Thirteen numbers. vmstat is itself one of the tasks it counts — the syscall
    // server that generated the file is runnable and the stepper is parked on its
    // reply — so `r` and `p` cannot be nought while it is the one asking.
    const cells = (vm[2] || "").trim().split(/ +/).map(Number);
    if (cells.length !== 13 || cells.some((n) => !Number.isFinite(n)))
        fail(`vmstat's row is not thirteen numbers: ${JSON.stringify(vm[2])}`);
    if (!(cells[0] >= 1 && cells[3] >= 1))
        fail(`vmstat counted itself out of its own row: ${JSON.stringify(vm[2])}`);
    if (!(cells[4] > 0 && cells[5] > 0))
        fail(`vmstat reported no heap: ${JSON.stringify(vm[2])}`);

    // -s is the same file, one counter per line with what it means. Its totals
    // are cumulative, so a syscall count since boot cannot be nought.
    s = submit("clear", 1185.6);
    s = submit("vmstat -s | grep syscalls", 1185.7);
    const sum = rows(s).filter((line) => line.includes("syscalls"));
    console.error("VMSTAT-SUM:\n" + rows(s).join("\n"));
    if (!sum.some((line) => /^ *[1-9]\d* syscalls parked and answered$/.test(line)))
        fail(`vmstat -s did not total the syscalls: ${JSON.stringify(rows(s))}`);

    s = submit("vmstat -s 1", 1185.8);
    if (!rows(s).includes("usage: vmstat [-s] [-m] [-w <secs>] [-c <count>] [<secs> [<count>]]"))
        fail(`vmstat -s with an interval printed ${JSON.stringify(rows(s))}`);

    // BSD's other spelling of the same thing, and -c counts the first row, so
    // one repetition never sleeps.
    s = submit("clear", 1185.9);
    s = submit("vmstat -w 1 -c 1", 1186.1);
    const once = rows(s).filter((line) => line && !line.includes("$"));
    if (once.length !== 3)
        fail(`vmstat -w 1 -c 1 printed ${once.length} lines, expected a header and a row`);

    // -m says the interval is milliseconds.
    s = submit("clear", 1185.95);
    s = submit("vmstat -m -w 1 -c 1", 1186.05);
    const milli = rows(s).filter((line) => line && !line.includes("$"));
    if (milli.length !== 3)
        fail(`vmstat -m -w 1 -c 1 printed ${milli.length} lines, expected a header and a row`);

    // Two rows an interval apart. The interval is a second, so the second row
    // needs a tick past the deadline the sleep put on the timer queue — which is
    // why this is not one submit.
    s = submit("clear", 1186.2);
    type("vmstat 1 2");
    press(KEY.ENTER);
    run(1186.3);
    run(2186.4); // past the second the first row asked to wait
    s = screen();
    const paced = rows(s).filter((line) => line && !line.includes("$"));
    console.error("VMSTAT-PACED:\n" + paced.join("\n"));
    if (paced.length !== 4)
        fail(`vmstat 1 2 printed ${JSON.stringify(paced)}, expected a header and two rows`);
    // The interval came from the file's own clock, so the second row's rates are
    // over ten milliseconds rather than over the whole uptime.
    if (paced[2] === paced[3])
        fail(`vmstat's second row repeated the first: ${JSON.stringify(paced)}`);

    regrid(60, 16, "the resize after ps failed");
}

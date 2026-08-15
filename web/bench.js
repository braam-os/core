// The tier measurement, doc/TODO.md T1 and T5. Boots the same kernel against
// three boot archives — the one that ships, and the two twins `make bench`
// packs, one tier at each end — runs the same four workloads against each, and
// reports.
//
// It is a page and not a Node driver on purpose: the numbers wanted are a
// browser's postMessage, its microtask queue and its setTimeout clamp.

import { mount } from "./braam.js";
import { pasted } from "./keys.js";

const NAMED = 0x110000; // src/kernel/key.h, in order
const ENTER = NAMED;
const CTRL = 2;

const params = new URLSearchParams(location.search);
const num = (name, fallback) => Number(params.get(name) || fallback);

const POLL_MS = num("poll", 8);
const RUNS = num("runs", 10);
const WARM = num("warm", 2);
const KEYS = num("keys", 40);
const BURST = num("burst", 64);

// The shell's own file, the largest thing in the archive at ~86 KB against a
// 512-byte SYS_CHUNK. The lever is the same command over eight files rather
// than one, and they have to be eight *different* files: one open per path is
// all the VFS allows, whoever asks (Concept.md §5.2).
const BIG = "/bin/sh";
const EIGHT = ["sh", "edit", "less", "grep", "ls", "cat", "wc", "chat"]
    .map((n) => `/bin/${n}`).join(" ");

// `wc` prints one total line however many operands it was given.
const COUNTS = /^\d+ \d+ \d+$/m;

const ARMS = {
    t2: { label: "tier 2, every program", bundle: "./bundle2.bin", tier3: false },
    t3: { label: "tier 3, sh included", bundle: "./bundle3.bin", tier3: true },
    t3nosh: { label: "tier 3, sh at tier 2 — as shipped", bundle: "./bundle.bin", tier3: true },
};

// Each arm twice, the second pass in reverse: JIT warm-up and thermal drift
// both run one way through a session, and two orderings cancel them.
const ORDER = ["t2", "t3", "t3nosh", "t3nosh", "t3", "t2"];

const WORKLOADS = [
    { id: "W0", cmd: "true", want: null, procs: 1 },
    { id: "W1", cmd: `wc ${BIG}`, want: COUNTS, procs: 1 },
    { id: "W2", cmd: `wc ${EIGHT}`, want: COUNTS, procs: 1 },
    { id: "W3", cmd: `cat ${BIG} | cat | wc`, want: COUNTS, procs: 3 },
];

// ------------------------------------------------------------------ numbers

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function median(xs) {
    if (!xs.length)
        return 0;
    const s = [...xs].sort((a, b) => a - b);
    const m = s.length >> 1;
    return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
}

function quartile(xs, q) {
    if (!xs.length)
        return 0;
    const s = [...xs].sort((a, b) => a - b);
    const at = (s.length - 1) * q;
    const lo = Math.floor(at);
    return s[lo] + (s[Math.ceil(at)] - s[lo]) * (at - lo);
}

const iqr = (xs) => quartile(xs, 0.75) - quartile(xs, 0.25);

// Whether the tier-2 figure is a measurement or the clock's own step: without
// cross-origin isolation performance.now() is coarse, and Braam ships no
// COOP/COEP headers by design.
function clockStep() {
    let min = Infinity;
    for (let i = 0; i < 200000; i++) {
        const a = performance.now();
        const b = performance.now();
        if (b > a && b - a < min)
            min = b - a;
    }
    return min === Infinity ? 0 : min;
}

// ------------------------------------------------------------------ one boot

function boot(bundle) {
    const canvas = document.createElement("canvas");
    canvas.id = "screen";
    document.getElementById("stage").replaceChildren(canvas);

    const logged = [];
    const waiting = { stats: [], text: [] };
    let asked = 0;

    const term = mount({
        canvas,
        bundleUrl: bundle,
        persist: false,
        onLog: (text) => logged.push(text),
        onError: (text) => logged.push(`error: ${text}`),
    });

    // braam.js keeps its own onmessage; this listens beside it.
    term.worker.addEventListener("message", ({ data }) => {
        if (!data)
            return;
        if (data.kind === "stats" && waiting.stats.length)
            waiting.stats.shift()(data);
        // A selection with no id on it is deselect()'s or the mouse's, not an
        // answer — and an answer may legitimately be empty.
        else if (data.kind === "selection" && data.id && waiting.text.length)
            waiting.text.shift()(data.text);
    });

    // A worker that never answers is a hang with nothing to report, which is
    // the one failure a harness must not have.
    const ask = (queue, msg) => Promise.race([
        new Promise((resolve) => {
            waiting[queue].push(resolve);
            term.worker.postMessage({ ...msg, id: ++asked });
        }),
        new Promise((_, reject) => setTimeout(
            () => reject(new Error(`the worker never answered ${msg.kind}`)), 15000)),
    ]);

    return {
        term,
        logged,
        stats: (reset) => ask("stats", { kind: "stats", reset: !!reset }),
        screen: () => ask("text", { kind: "selectall" }),
        key: (code, mods = 0) => term.worker.postMessage({ kind: "key", code, mods }),
        paste: (text) => term.worker.postMessage({ kind: "paste", codes: pasted(text) }),
        dispose: () => term.dispose(),
    };
}

// The prompt is the last status in red, the cwd's basename, then the $. A run
// whose command failed leaves the status in it, and is not a measurement of
// what it was supposed to measure.
const PROMPT = /^(\[\d+\] )?[^ ]+ \$$/;
const OK = /^[^ [][^ ]* \$$/;
const last = (text) => text.split("\n").pop();
const at_prompt = (text) => PROMPT.test(last(text));

async function ready(vm) {
    let booted = false;
    for (let i = 0; i < 500 && !booted; i++) {
        booted = !!(await vm.stats()).proc;
        if (!booted)
            await sleep(20);
    }
    // Answered before the kernel exists, so a null proc is boot itself not
    // finishing — on WebKit, an OPFS handle the previous document's worker has
    // not let go of yet.
    if (!booted)
        throw new Error(`the kernel never finished booting. log: ${JSON.stringify(vm.logged)}`);

    let seen = "";
    for (let i = 0; i < 500; i++) {
        seen = await vm.screen();
        if (at_prompt(seen))
            return;
        await sleep(20);
    }
    throw new Error(`no prompt. screen: ${JSON.stringify(seen)}. `
                    + `log: ${JSON.stringify(vm.logged)}`);
}

// Nothing running and nothing repainting, twice over: the shell's epilogue
// reaps an exited process a tick after its output has landed.
async function quiesce(vm, live) {
    let mark = null;
    let still = 0;
    for (let i = 0; i < 4000; i++) {
        const s = await vm.stats();
        const now = `${s.proc.live}:${s.repaints}:${s.keys.length}`;
        const settled = now === mark && (live === undefined || s.proc.live === live);
        still = settled ? still + 1 : 0;
        if (still >= 2)
            return s;
        mark = now;
        await sleep(POLL_MS);
    }
    throw new Error("never settled");
}

const DELTA = ["calls2", "calls3", "steps2", "steps3", "spawned", "compiled",
               "hired", "reused", "terminated", "broke"];
const DEFER = ["micro_n", "micro_ms", "timer_n", "timer_ms"];

function delta(a, b) {
    const out = {};
    for (const k of DELTA)
        out[k] = b.proc[k] - a.proc[k];
    for (const k of DEFER)
        out[k] = b.defer[k] - a.defer[k];
    out.trips = out.calls2 + out.calls3;
    return out;
}

// One timed command. The two ends are stamped in the worker — the Enter that
// started it, and the repaint of the prompt that came back — so what the page
// polls for is only *whether* it is over, never when. A poll costs a message
// and would otherwise put its own interval into every number.
async function once(vm, w) {
    trace(`  ${w.id} settling`);
    const base = (await quiesce(vm)).proc.live;

    trace(`  ${w.id} typing`);
    vm.paste(w.cmd);
    await quiesce(vm, base);

    trace(`  ${w.id} entering`);
    const a = await vm.stats();
    vm.key(ENTER);

    let started = false;
    let still = 0;
    for (let i = 0; i < 4000 && still < 2; i++) {
        await sleep(POLL_MS);
        const s = await vm.stats();
        started = started || s.proc.spawned - a.proc.spawned >= w.procs;
        still = started && s.proc.live === base ? still + 1 : 0;
    }
    if (still < 2)
        throw new Error(`${w.cmd}: never finished`);

    trace(`  ${w.id} ran`);
    const b = await quiesce(vm, base);
    const text = await vm.screen();
    const good = OK.test(last(text)) && (!w.want || w.want.test(text));
    return { ms: b.last_present - b.last_key, d: delta(a, b), good };
}

// The keystroke, paced: one key in flight, so the 64-slot ring is never the
// thing being timed. Two numbers come out of it, and they are not the same one
// — a repaint is four syscalls, and the first of them damages the grid.
//
//   `first` — key to the first repaint, which is when the terminal has visibly
//             answered, and is what the worker's one-slot sampler holds.
//   `echo`  — key to the last repaint of that keystroke, the whole of it.
//
// Under Blink they are within half again of each other; under Gecko the second
// is forty times the first, so reporting either alone would mislead.
async function paced(vm, n) {
    await vm.stats(true);
    const first = [];
    const echo = [];
    for (let i = 0; i < n; i++) {
        vm.key(0x78); // 'x'
        const s = await quiesce(vm);
        if (s.keys.length !== i + 1)
            throw new Error("a keystroke was never painted");
        first.push(s.keys[i]);
        echo.push(s.last_present - s.last_key);
    }
    vm.key(0x63, CTRL); // abandon the line; at the prompt ^C kills nothing
    await quiesce(vm);
    return { first, echo };
}

// And the same keys posted back to back, which is a different measurement: the
// sampler holds one stamp, so this is wall time for the run and a count of what
// survived the ring rather than a per-key latency.
async function burst(vm, n) {
    const a = await vm.stats(true);
    for (let i = 0; i < n; i++)
        vm.key(0x61); // 'a'
    const b = await quiesce(vm);
    const line = last(await vm.screen());
    vm.key(0x63, CTRL);
    await quiesce(vm);
    return { sent: n, echoed: (line.match(/a/g) || []).length, ms: b.last_present - a.now };
}

// ------------------------------------------------------------------- the run

// Progress goes to the page and to the collector both: a browser whose console
// nobody is reading is the usual case here, and a run that stops has to say
// where. The watchdog below turns a stall into a diagnostic rather than a tab
// that sits there.
let said = "";
let said_at = 0;

function note(text, cls) {
    const line = document.createElement("div");
    if (cls)
        line.className = cls;
    line.textContent = text;
    document.getElementById("log").appendChild(line);
    line.scrollIntoView();

    said = text;
    said_at = performance.now();
    post("/note", { note: text, agent: navigator.userAgent });
}

const trace = params.get("trace") === "1" ? note : () => {};

function post(where, body) {
    return fetch(where, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify(body),
    }).catch(() => {});
}

const STALL_MS = 60000;

function watchdog() {
    const timer = setInterval(() => {
        if (said_at && performance.now() - said_at > STALL_MS) {
            clearInterval(timer); // once, or the report says it about itself
            const where = said;
            note(`stalled for ${STALL_MS / 1000} s after: ${where}`, "error");
            post("/bench", { failed: `stalled after: ${where}`, agent: navigator.userAgent });
        }
    }, 5000);
}

async function arm(id, acc) {
    const spec = ARMS[id];
    note(`${id}: booting ${spec.bundle}`);
    const vm = boot(spec.bundle);
    try {
        await ready(vm);
        note(`${id}: at the prompt`);
        const first = await vm.stats();
        acc.workers = first.proc.workers;
        if (spec.tier3 && !first.proc.workers)
            throw new Error("no nested workers: a tier-3 arm would measure tier 2 twice");
        trace(`${id}: workers ${first.proc.workers}, pooled ${first.proc.pooled}, `
              + `live ${first.proc.live}, spawned ${first.proc.spawned}, `
              + `broke ${first.proc.broke}`);

        for (const w of WORKLOADS) {
            note(`${id} ${w.id}: ${w.cmd}`);
            for (let i = 0; i < WARM; i++)
                await once(vm, w);
            for (let i = 0; i < RUNS; i++) {
                const r = await once(vm, w);
                if (!r.good) {
                    acc.bad = (acc.bad || 0) + 1;
                    continue;
                }
                const into = acc.work[w.id] || (acc.work[w.id] = { ms: [], d: [] });
                into.ms.push(r.ms);
                into.d.push(r.d);
            }
            note(`${id} ${w.id}: ${median(acc.work[w.id].ms).toFixed(1)} ms`);
        }

        await paced(vm, WARM);
        const k = await paced(vm, KEYS);
        acc.keys.push(...k.first);
        acc.echo.push(...k.echo);
        acc.burst.push(await burst(vm, BURST));
        note(`${id} keys: ${median(acc.echo).toFixed(2)} ms to the full echo`);

        const last = await vm.stats();
        acc.defer = last.defer;
        acc.version = acc.version || (vm.logged.join("\n").match(/braam \S+/) || [""])[0];
    } finally {
        vm.dispose();
        // Long enough for the worker, its nested ones and the OPFS handles they
        // hold to go: WebKit will not boot the next kernel until they have, and
        // the next kernel is a page load away.
        await sleep(2000);
    }
}

function summarise(acc) {
    const work = {};
    for (const [id, v] of Object.entries(acc.work)) {
        const sum = (k) => median(v.d.map((d) => d[k]));
        work[id] = {
            n: v.ms.length,
            median: median(v.ms),
            iqr: iqr(v.ms),
            min: Math.min(...v.ms),
            trips: sum("trips"),
            steps: sum("steps2") + sum("steps3"),
            spawned: sum("spawned"),
            hired: sum("hired"),
            reused: sum("reused"),
            terminated: sum("terminated"),
            micro_n: sum("micro_n"),
            micro_ms: sum("micro_ms"),
            timer_n: sum("timer_n"),
            timer_ms: sum("timer_ms"),
        };
    }

    // ΔT/ΔN over the one pair that differs in nothing but how many round trips
    // it makes. Everything one-off — the spawn, the compile-cache hit, the
    // instantiate, the exit, the prompt redraw — subtracts out.
    let us = null;
    if (work.W1 && work.W2 && work.W2.trips > work.W1.trips)
        us = (work.W2.median - work.W1.median) * 1000 / (work.W2.trips - work.W1.trips);

    return {
        label: ARMS[acc.id].label,
        workers: acc.workers,
        tainted: !!acc.tainted,
        bad: acc.bad || 0,
        work,
        us_per_trip: us,
        key_ms: median(acc.keys),
        key_iqr: iqr(acc.keys),
        // performance.now() steps in 0.1 ms without cross-origin isolation, so
        // one sample is quantised and the median is one of two or three values.
        // The mean over a few dozen is not.
        key_mean: acc.keys.reduce((a, b) => a + b, 0) / (acc.keys.length || 1),
        key_n: acc.keys.length,
        echo_ms: median(acc.echo),
        echo_iqr: iqr(acc.echo),
        echo_mean: acc.echo.reduce((a, b) => a + b, 0) / (acc.echo.length || 1),
        burst: acc.burst,
        defer: acc.defer,
    };
}

function table(report) {
    const rows = [["arm", "job", "n", "med ms", "iqr", "trips", "steps", "micro",
                   "timer", "timer ms", "proc", "hire", "reuse", "term"]];
    for (const [id, a] of Object.entries(report.arms))
        for (const [w, v] of Object.entries(a.work))
            rows.push([id, w, v.n, v.median.toFixed(1), v.iqr.toFixed(1), v.trips,
                       v.steps, v.micro_n, v.timer_n, v.timer_ms.toFixed(1),
                       v.spawned, v.hired, v.reused, v.terminated].map(String));

    const wide = rows[0].map((_, i) => Math.max(...rows.map((r) => r[i].length)));
    const out = rows.map((r) => r.map((c, i) => c.padStart(wide[i])).join("  "));
    out.splice(1, 0, wide.map((n) => "-".repeat(n)).join("  "));

    out.push("");
    for (const [id, a] of Object.entries(report.arms)) {
        const us = a.us_per_trip === null ? "n/a" : `${a.us_per_trip.toFixed(1)} us`;
        const b = a.burst[0] || {};
        out.push(`${id}: ${us}/round trip, key ${a.key_mean.toFixed(3)} ms to first paint, `
                 + `${a.echo_mean.toFixed(3)} to the echo (n ${a.key_n}), `
                 + `burst ${b.echoed}/${b.sent} in ${(b.ms || 0).toFixed(0)} ms`);
    }
    return out.join("\n");
}

// One arm per page load, carried in sessionStorage. Six mounts in one document
// is more than WebKit will give: the third worker never starts, and the arm
// after it measures nothing. A reload costs the JIT state that made a single
// document attractive, and buys a run that finishes in every browser — and the
// numbers that matter are differences taken *within* an arm, which a reload
// between arms cannot disturb.
const KEPT = "braam-bench";

const kept = () => {
    try {
        return JSON.parse(sessionStorage.getItem(KEPT) || "{}");
    } catch {
        return {};
    }
};

function keep(state) {
    sessionStorage.setItem(KEPT, JSON.stringify(state));
}

function onward(step) {
    const next = new URLSearchParams(params);
    next.set("step", String(step));
    location.replace(`${location.pathname}?${next}`);
}

async function main() {
    if (document.visibilityState !== "visible") {
        note("this tab is not visible; a background tab throttles the timers "
             + "this measures. Bring it to the front and reload.", "error");
        return;
    }

    const step = Number(params.get("step") || 0);
    const state = step ? kept() : { accs: {}, tainted: false, tries: {} };
    state.tries = state.tries || {};

    // Per pass, not per session: eight minutes of six page loads with a person
    // at the machine will lose focus once, and one arm's numbers going soft is
    // not a reason to throw the other five away.
    let hidden = false;
    document.addEventListener("visibilitychange", () => {
        hidden = hidden || document.visibilityState !== "visible";
    });

    if (step < ORDER.length) {
        watchdog();
        note(`pass ${step + 1} of ${ORDER.length}: ${ORDER[step]} — `
             + `runs ${RUNS}, warm-ups ${WARM}, do not switch away`);
        const id = ORDER[step];
        const acc = state.accs[id]
            || (state.accs[id] = { id, work: {}, keys: [], echo: [], burst: [] });

        // A kernel that never finishes booting is the one failure that recurs,
        // and a fresh document is the only reliable way out of it: the handles
        // the last one held go with it. Retried here rather than by hand.
        const tries = (state.tries[step] || 0) + 1;
        try {
            await arm(id, acc);
        } catch (e) {
            if (tries >= 4)
                throw e;
            // Kept, not discarded: the failure is at boot, before this pass has
            // contributed anything, and the arm's other pass may already have.
            state.tries[step] = tries;
            keep(state);
            note(`${id}: ${e.message} — retrying (${tries})`, "error");
            await sleep(3000);
            onward(step);
            return;
        }

        if (hidden) {
            acc.tainted = true;
            state.tainted = true;
            note(`${id}: this pass lost focus and is marked`, "error");
        }
        keep(state);
        onward(step + 1);
        return;
    }

    const accs = state.accs;
    const report = {
        agent: navigator.userAgent,
        cores: navigator.hardwareConcurrency || 0,
        isolated: !!self.crossOriginIsolated,
        clock_step_ms: clockStep(),
        version: Object.values(accs).map((a) => a.version).find(Boolean) || "",
        runs: RUNS,
        warm: WARM,
        poll_ms: POLL_MS,
        tainted: state.tainted,
        arms: Object.fromEntries(Object.entries(accs).map(([id, a]) => [id, summarise(a)])),
    };

    document.getElementById("table").textContent = table(report);
    const soft = Object.entries(report.arms).filter(([, a]) => a.tainted).map(([id]) => id);
    if (soft.length)
        note(`these passes lost focus and are marked in the report: ${soft.join(", ")}`, "error");
    await post("/bench", report);
    note("posted to the collector");
}

main().catch((e) => {
    const text = e && e.message ? e.message : String(e);
    note(`bench: ${text}`, "error");
    // The collector is running in a terminal somebody is watching; a run that
    // died in the browser should say so there too.
    fetch("/bench", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ failed: text, agent: navigator.userAgent }),
    }).catch(() => {});
});

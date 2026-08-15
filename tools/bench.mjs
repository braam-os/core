// Serves build/web/ for web/bench.html and collects what it posts back, in
// plain Node with no dependencies. The measurement itself is the page's; this
// is somewhere for its numbers to land — doc/TODO.md T1.
//
// It keeps serving after a report, so the same URL can be handed to one browser
// after another; each writes build/bench-<engine>.json.

import { createServer } from "node:http";
import { readFile, writeFile } from "node:fs/promises";
import { extname, join, normalize } from "node:path";

const PORT = Number(process.env.BRAAM_BENCH_PORT || 8082);
const ROOT = process.env.BRAAM_BENCH_ROOT || "build/web";
const OUT = process.env.BRAAM_BENCH_OUT || "build";

const TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".json": "application/json",
    // Without this, instantiateStreaming falls back to a buffered compile.
    ".wasm": "application/wasm",
    ".bin": "application/octet-stream",
    ".css": "text/css; charset=utf-8",
};

// Enough to tell three engines apart, and no more: Blink names itself last.
function engine(agent = "") {
    if (/Firefox\//.test(agent))
        return "firefox";
    if (/Vivaldi\//.test(agent))
        return "vivaldi";
    if (/Chrome\//.test(agent))
        return "chrome";
    if (/Safari\//.test(agent))
        return "safari";
    return "unknown";
}

function pad(rows) {
    const wide = rows[0].map((_, i) => Math.max(...rows.map((r) => String(r[i]).length)));
    return rows.map((r) => r.map((c, i) => String(c).padStart(wide[i])).join("  "));
}

function table(report) {
    const rows = [["arm", "job", "n", "med ms", "iqr", "trips", "steps", "micro",
                   "timer", "timer ms", "proc", "hire", "reuse", "term"]];
    for (const [id, a] of Object.entries(report.arms))
        for (const [w, v] of Object.entries(a.work))
            rows.push([id, w, v.n, v.median.toFixed(1), v.iqr.toFixed(1), v.trips,
                       v.steps, v.micro_n, v.timer_n, v.timer_ms.toFixed(1),
                       v.spawned, v.hired, v.reused, v.terminated]);

    const out = pad(rows);
    out.splice(1, 0, "-".repeat(out[0].length));

    out.push("");
    for (const [id, a] of Object.entries(report.arms)) {
        const us = a.us_per_trip === null ? "n/a" : `${a.us_per_trip.toFixed(1)} us`;
        const b = a.burst[0] || {};
        const w2 = a.work.W2 || {};
        out.push(`${id}  ${a.label}`);
        out.push(`    ${us}/round trip; key ${a.key_mean.toFixed(3)} ms to the first repaint, `
                 + `${a.echo_mean.toFixed(3)} ms to the full echo `
                 + `(median ${a.echo_ms.toFixed(2)}, iqr ${a.echo_iqr.toFixed(2)}, n ${a.key_n})`);
        out.push(`    burst ${b.echoed}/${b.sent} keys in ${(b.ms || 0).toFixed(0)} ms; `
                 + `workers ${a.workers}; discarded ${a.bad}`
                 + (a.tainted ? "; TAINTED, this pass lost focus" : ""));
        if (w2.micro_n || w2.timer_n)
            out.push(`    W2 step deferral: ${w2.micro_n} microtasks `
                     + `${(w2.micro_ms / (w2.micro_n || 1)).toFixed(3)} ms each, `
                     + `${w2.timer_n} timers ${(w2.timer_ms / (w2.timer_n || 1)).toFixed(3)} ms each`);
    }
    return out.join("\n");
}

async function report(body) {
    let r;
    try {
        r = JSON.parse(body);
    } catch {
        return "bench: the report did not parse";
    }
    if (r.failed) {
        console.log(`\n${engine(r.agent)}: the run failed — ${r.failed}`);
        return null;
    }

    const name = join(OUT, `bench-${engine(r.agent)}.json`);
    await writeFile(name, `${JSON.stringify(r, null, 2)}\n`);

    console.log(`\n${r.agent}`);
    console.log(`${r.version || "braam"}, ${r.cores} cores, `
                + `clock step ${r.clock_step_ms.toFixed(4)} ms, `
                + `isolated ${r.isolated}, runs ${r.runs}`
                + (r.tainted ? ", TAINTED (the tab lost focus)" : ""));
    console.log(table(r));
    console.log(`\nwritten to ${name}`);
    return null;
}

// The page's progress line, so a run that stalls in a browser nobody has a
// console open on says where it stopped.
function noted(body) {
    try {
        const { note, agent } = JSON.parse(body);
        console.log(`${engine(agent)}: ${note}`);
    } catch {
        // a note is not worth failing over
    }
}

const server = createServer((req, res) => {
    if (req.method === "POST" && (req.url === "/bench" || req.url === "/note")) {
        const chunks = [];
        req.on("data", (c) => chunks.push(c));
        req.on("end", async () => {
            const body = Buffer.concat(chunks).toString("utf8");
            if (req.url === "/note") {
                noted(body);
                res.writeHead(204).end();
                return;
            }
            const bad = await report(body);
            res.writeHead(bad ? 400 : 204).end(bad || undefined);
        });
        return;
    }

    const url = new URL(req.url, "http://localhost");
    const want = url.pathname === "/" ? "/bench.html" : url.pathname;
    const path = join(ROOT, normalize(want));
    if (!path.startsWith(ROOT)) {
        res.writeHead(403).end();
        return;
    }

    readFile(path).then((body) => {
        res.writeHead(200, {
            "content-type": TYPES[extname(path)] || "application/octet-stream",
            "cache-control": "no-store",
        }).end(body);
    }, () => {
        res.writeHead(404).end();
    });
});

server.listen(PORT, () => {
    console.log(`bench: serving ${ROOT} at http://localhost:${PORT}/`);
    console.log("bench: point one browser at a time at it; ^C when done");
});

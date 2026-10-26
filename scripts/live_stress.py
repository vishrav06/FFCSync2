#!/usr/bin/env python3
"""Live FFCSync stress test with charts that update in the browser.

Runs the real server in each requested mode, one after another. For each mode,
it repeatedly launches --clients client processes for --seconds seconds and
streams throughput and latency to a local web page (no internet needed).
When a mode's phase ends, the server is stopped with SIGTERM and checked:
the invariant check must pass, and replaying its admission log through
ffcsync_seq must reproduce results.csv and final_state.csv byte for byte.

    python3 scripts/live_stress.py                       # open the printed URL, press Start
    python3 scripts/live_stress.py --autostart --seconds 15
    python3 scripts/live_stress.py --modes gate:4 inline committer --clients 4 --max-inflight 64

Mode names: gate:N (N workers + commit gate), inline (--workers 0),
committer (--mode committer). Outputs of each run: results/live/<mode>/.
Only the Python standard library is used.
"""
import argparse
import csv
import filecmp
import http.server
import json
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
POOL = 4   # workload files per client, used in rotation

MODES = {
    "inline": ("inline server (workers 0)", ["--workers", "0"]),
    "committer": ("single committer", ["--workers", "1", "--mode", "committer"]),
}


def mode_spec(name):
    if name in MODES:
        return name, *MODES[name]
    if name.startswith("gate:") and name[5:].isdigit() and int(name[5:]) > 0:
        n = int(name[5:])
        return name, f"gate, {n} worker{'s' if n > 1 else ''}", ["--workers", str(n), "--mode", "gate"]
    raise argparse.ArgumentTypeError(f"unknown mode '{name}' (use gate:N, inline or committer)")


class Run:
    """Shared state between the runner thread and the HTTP handlers."""

    def __init__(self, args):
        self.args = args
        self.lock = threading.Lock()
        self.start_requested = threading.Event()
        self.stop = threading.Event()
        self.reset()

    def reset(self):
        self.state = {
            "status": "waiting",   # waiting | running | done | error
            "message": "Press Start to begin.",
            "current": None,
            "config": {"clients": self.args.clients, "requests_per_client": self.args.requests,
                       "max_inflight": self.args.max_inflight, "queue_capacity": self.args.queue_capacity,
                       "seconds": self.args.seconds},
            "modes": [{"id": m[0], "label": m[1], "status": "pending", "points": [],
                       "requests": 0, "accepted": 0, "rejected": 0,
                       "invariants": None, "oracle": None} for m in self.args.modes],
        }

    def update(self, fn):
        with self.lock:
            fn(self.state)

    def snapshot(self):
        with self.lock:
            return json.dumps(self.state)


def generate_pool(args, tmp):
    files = []
    for c in range(args.clients):
        mine = []
        for k in range(POOL):
            f = tmp / f"c{c}_{k}.csv"
            subprocess.run([sys.executable, "-I", str(ROOT / "scripts/gen_workload.py"), "--data", str(args.data),
                            "--n", str(args.requests), "--seed", str(1000 + c * POOL + k),
                            "--prefix", f"{chr(ord('A') + c)}{k}_", "--out", str(f)],
                           check=True, capture_output=True)
            mine.append(f)
        files.append(mine)
    return files


def wait_for_socket(sock, proc):
    for _ in range(300):
        if sock.exists():
            return True
        if proc.poll() is not None:
            return False
        time.sleep(0.01)
    return False


def one_round(args, sock, files, rnd, tmp):
    """Launches every client once; returns (throughput, avg_us, p95_us, received, accepted)."""
    outs = [tmp / f"out{c}.csv" for c in range(args.clients)]
    procs = [subprocess.Popen([str(args.build / "ffcsync_client"), "--socket", str(sock),
                               "--requests", str(files[c][rnd % POOL]), "--quiet",
                               "--max-inflight", str(args.max_inflight), "--out", str(outs[c])],
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
             for c in range(args.clients)]
    total_ms = []
    for p in procs:
        _, err = p.communicate()
        if p.returncode != 0:
            raise RuntimeError(f"client failed: {err.strip()}")
        fields = dict(kv.split("=", 1) for kv in err.split() if "=" in kv)
        total_ms.append(float(fields["total_ms"]))
    lat, accepted = [], 0
    for o in outs:
        with o.open() as f:
            for row in csv.DictReader(f):
                lat.append(int(row["latency_us"]))
                accepted += row["status"] == "ACCEPTED"
    lat.sort()
    p95 = lat[max(0, -(-len(lat) * 95 // 100) - 1)]
    rps = len(lat) / (max(total_ms) / 1000) if max(total_ms) > 0 else 0
    return rps, statistics.fmean(lat), p95, len(lat), accepted


def run_mode(run, idx, mode, files, tmp):
    args = run.args
    mode_id, label, flags = mode
    sock = tmp / f"{mode_id.replace(':', '_')}.sock"
    out = args.out / mode_id.replace(":", "_")
    shutil.rmtree(out, ignore_errors=True)

    def set_mode(**kw):
        run.update(lambda s: s["modes"][idx].update(kw))

    run.update(lambda s: s.update(current=idx, message=f"Running: {label}"))
    set_mode(status="running")
    server = subprocess.Popen([str(args.build / "ffcsync_server"), "--data", str(args.data), "--socket", str(sock),
                               "--queue-capacity", str(args.queue_capacity), "--out", str(out), *flags],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_for_socket(sock, server):
            raise RuntimeError(f"{label}: server did not start")
        t0 = time.monotonic()
        rnd = 0
        while time.monotonic() - t0 < args.seconds and not run.stop.is_set():
            rps, avg, p95, n, acc = one_round(args, sock, files, rnd, tmp)
            t = round(time.monotonic() - t0, 3)
            rnd += 1

            def add(s, t=t, rps=rps, avg=avg, p95=p95, n=n, acc=acc):
                m = s["modes"][idx]
                m["points"].append({"t": t, "rps": round(rps), "avg": round(avg), "p95": p95})
                m["requests"] += n
                m["accepted"] += acc
                m["rejected"] += n - acc
            run.update(add)
    finally:
        server.send_signal(signal.SIGTERM)
        code = server.wait()

    set_mode(status="checking")
    run.update(lambda s: s.update(message=f"Checking {label} against the sequential replay..."))
    summary = (out / "summary.txt").read_text() if (out / "summary.txt").exists() else ""
    invariants = code == 0 and "invariants: OK" in summary
    oracle = out / "oracle"
    subprocess.run([str(args.build / "ffcsync_seq"), "replay", "--data", str(args.data),
                    "--admission-log", str(out / "admission.csv"), "--out", str(oracle)],
                   check=True, capture_output=True)
    same = all(filecmp.cmp(out / f, oracle / f, shallow=False) for f in ("results.csv", "final_state.csv"))
    set_mode(status="done", invariants=invariants, oracle=same)
    return invariants and same


def runner(run):
    while True:
        run.start_requested.wait()
        run.start_requested.clear()
        if run.stop.is_set():
            return
        run.update(lambda s: s.update(status="running", message="Generating workloads..."))
        ok = True
        try:
            with tempfile.TemporaryDirectory(prefix="ffcsync_live_") as t:
                tmp = Path(t)
                files = generate_pool(run.args, tmp)
                for idx, mode in enumerate(run.args.modes):
                    if run.stop.is_set():
                        return
                    ok &= run_mode(run, idx, mode, files, tmp)
            msg = ("All modes passed: invariants OK and results identical to the sequential replay."
                   if ok else "A correctness check FAILED - see the table below.")
            run.update(lambda s: s.update(status="done", current=None, message=msg))
            print(msg, flush=True)
        except Exception as e:   # shown on the page, the script keeps serving
            run.update(lambda s: s.update(status="error", current=None, message=f"Error: {e}"))
            print(f"error: {e}", file=sys.stderr, flush=True)


def make_handler(run):
    page = PAGE.encode()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/":
                self.send(200, page, "text/html; charset=utf-8")
            elif self.path == "/state":
                self.send(200, run.snapshot().encode(), "application/json")
            else:
                self.send(404, b"not found", "text/plain")

        def do_POST(self):
            if self.path != "/start":
                return self.send(404, b"not found", "text/plain")
            with run.lock:
                busy = run.state["status"] == "running"
            if not busy:
                with run.lock:
                    run.reset()
                    run.state["status"] = "running"
                run.start_requested.set()
            self.send(200, b"{}", "application/json")

    return Handler


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--build", type=Path, default=ROOT / "build")
    p.add_argument("--data", type=Path, default=ROOT / "data")
    p.add_argument("--out", type=Path, default=ROOT / "results/live")
    p.add_argument("--modes", type=mode_spec, nargs="+",
                   default=[mode_spec(m) for m in ("gate:4", "inline", "committer")])
    p.add_argument("--seconds", type=float, default=15, help="duration of each mode's phase")
    p.add_argument("--clients", type=int, default=4, help="client processes per round")
    p.add_argument("--requests", type=int, default=15000, help="requests per client per round")
    p.add_argument("--max-inflight", type=int, default=64)
    p.add_argument("--queue-capacity", type=int, default=64)
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--autostart", action="store_true", help="start immediately instead of waiting for the button")
    args = p.parse_args()
    for exe in ("ffcsync_server", "ffcsync_client", "ffcsync_seq"):
        if not (args.build / exe).exists():
            sys.exit(f"missing {args.build / exe}: build first (cmake -S . -B build && cmake --build build -j)")
    args.out.mkdir(parents=True, exist_ok=True)

    run = Run(args)
    threading.Thread(target=runner, args=(run,), daemon=True).start()
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(run))
    print(f"Live stress test: open http://localhost:{args.port}/ in a browser"
          f"{' (starting now)' if args.autostart else ' and press Start'}. Ctrl-C to quit.", flush=True)
    if args.autostart:
        with run.lock:
            run.state["status"] = "running"
        run.start_requested.set()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        run.stop.set()
        run.start_requested.set()
        httpd.server_close()


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>FFCSync Live Stress</title>
<style>
:root {
  color-scheme: light;
  --surface: #fcfcfb; --card: #ffffff; --border: #e4e3df; --grid: #ecebe7;
  --text: #0b0b0b; --text2: #52514e; --muted: #7a7974;
  --s1: #2a78d6; --s2: #eb6834; --s3: #1baf7a; --s4: #eda100;
  --good: #0f7b3e; --bad: #c62828; --accent: #2a78d6;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    color-scheme: dark;
    --surface: #1a1a19; --card: #222220; --border: #3a3a37; --grid: #2e2e2b;
    --text: #f0efec; --text2: #c3c2b7; --muted: #8f8e86;
    --s1: #3987e5; --s2: #d95926; --s3: #199e70; --s4: #c98500;
    --good: #4cbb7a; --bad: #ef6b6b; --accent: #3987e5;
  }
}
:root[data-theme="dark"] {
  color-scheme: dark;
  --surface: #1a1a19; --card: #222220; --border: #3a3a37; --grid: #2e2e2b;
  --text: #f0efec; --text2: #c3c2b7; --muted: #8f8e86;
  --s1: #3987e5; --s2: #d95926; --s3: #199e70; --s4: #c98500;
  --good: #4cbb7a; --bad: #ef6b6b; --accent: #3987e5;
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--surface); color: var(--text);
  font: 15px/1.45 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
main { max-width: 1400px; margin: 0 auto; padding: 20px 16px 40px; }
header { display: flex; flex-wrap: wrap; align-items: center; gap: 12px 20px; margin-bottom: 6px; }
h1 { font-size: 22px; margin: 0; letter-spacing: -0.01em; }
.sub { color: var(--text2); font-size: 13px; margin: 0 0 16px; }
button { font: inherit; font-weight: 600; padding: 8px 18px; border-radius: 8px; border: 0;
  background: var(--accent); color: #fff; cursor: pointer; }
button:disabled { opacity: .45; cursor: default; }
.status { font-size: 14px; color: var(--text2); }
.legend { display: flex; flex-wrap: wrap; gap: 6px 18px; margin: 0 0 14px; font-size: 14px; }
.legend span { display: inline-flex; align-items: center; gap: 7px; color: var(--text); }
.legend i { width: 14px; height: 4px; border-radius: 2px; display: inline-block; }
.legend em { font-style: normal; color: var(--muted); font-size: 12px; }
.charts { display: grid; grid-template-columns: repeat(auto-fit, minmax(340px, 1fr)); gap: 14px; }
.card { background: var(--card); border: 1px solid var(--border); border-radius: 10px; padding: 12px 14px 8px; position: relative; }
.card h2 { font-size: 14px; font-weight: 600; margin: 0 0 2px; }
.card .hint { font-size: 12px; color: var(--muted); margin: 0 0 4px; }
svg { display: block; width: 100%; height: 260px; overflow: visible; }
svg text { fill: var(--text2); font-size: 11px; }
.tip { position: absolute; pointer-events: none; background: var(--card); border: 1px solid var(--border);
  border-radius: 8px; padding: 6px 9px; font-size: 12px; box-shadow: 0 4px 14px rgba(0,0,0,.12);
  display: none; white-space: nowrap; z-index: 2; }
.tip b { font-weight: 600; }
.tip i { width: 10px; height: 3px; border-radius: 2px; display: inline-block; margin-right: 6px; vertical-align: middle; }
h3 { font-size: 15px; margin: 22px 0 8px; }
.table-wrap { overflow-x: auto; }
table { border-collapse: collapse; width: 100%; background: var(--card); border: 1px solid var(--border);
  border-radius: 10px; overflow: hidden; font-size: 14px; }
th, td { padding: 8px 12px; text-align: right; border-bottom: 1px solid var(--border); white-space: nowrap; }
th:first-child, td:first-child { text-align: left; }
th { font-weight: 600; color: var(--text2); font-size: 12px; }
tr:last-child td { border-bottom: 0; }
.ok { color: var(--good); font-weight: 600; } .bad { color: var(--bad); font-weight: 600; }
.pend { color: var(--muted); }
.sw { width: 10px; height: 10px; border-radius: 2px; display: inline-block; margin-right: 8px; vertical-align: -1px; }
footer { color: var(--muted); font-size: 12px; margin-top: 14px; }
</style></head>
<body><main>
<header>
  <h1>FFCSync live stress test</h1>
  <button id="start">Start</button>
  <span class="status" id="status">Connecting...</span>
</header>
<p class="sub" id="config"></p>
<div class="legend" id="legend"></div>
<div class="charts">
  <div class="card"><h2>Throughput</h2><p class="hint">requests / second, higher is better</p><svg id="c-rps"></svg><div class="tip"></div></div>
  <div class="card"><h2>Average latency</h2><p class="hint">microseconds, lower is better</p><svg id="c-avg"></svg><div class="tip"></div></div>
  <div class="card"><h2>p95 latency</h2><p class="hint">microseconds, lower is better</p><svg id="c-p95"></svg><div class="tip"></div></div>
</div>
<h3>Results and correctness checks</h3>
<div class="table-wrap"><table>
  <thead><tr><th>Mode</th><th>Median throughput</th><th>Median avg latency</th><th>Median p95</th>
  <th>Requests</th><th>Accepted / rejected</th><th>Invariants</th><th>Matches sequential replay</th></tr></thead>
  <tbody id="rows"></tbody>
</table></div>
<footer>Each point is one round of all clients. Modes run one after another on the same machine. After each mode, the server is stopped and its admission log is replayed through the sequential engine; results and final state must be byte-identical.</footer>
</main>
<script>
const COLORS = ["--s1", "--s2", "--s3", "--s4"];
const css = v => getComputedStyle(document.documentElement).getPropertyValue(v).trim();
const fmtK = v => v >= 1e6 ? (v / 1e6).toFixed(v % 1e6 ? 2 : 0) + "M"
  : v >= 1000 ? (v / 1000).toFixed(v >= 100000 ? 0 : 1) + "k" : String(Math.round(v));
const fmtN = v => Math.round(v).toLocaleString("en-US");
const CHARTS = [
  {id: "c-rps", key: "rps", fmt: fmtK, unit: " req/s"},
  {id: "c-avg", key: "avg", fmt: fmtN, unit: " µs"},
  {id: "c-p95", key: "p95", fmt: fmtN, unit: " µs"},
];
let state = null;

function niceMax(v) {
  if (v <= 0) return 1;
  const p = Math.pow(10, Math.floor(Math.log10(v))), n = v / p;
  return (n <= 1 ? 1 : n <= 2 ? 2 : n <= 2.5 ? 2.5 : n <= 5 ? 5 : n <= 7.5 ? 7.5 : 10) * p;
}

function draw(chart) {
  const svg = document.getElementById(chart.id);
  const W = svg.clientWidth || 400, H = 260, L = 48, R = 14, T = 10, B = 26;
  const modes = state ? state.modes : [];
  const tMax = Math.max(state ? state.config.seconds : 10, ...modes.flatMap(m => m.points.map(p => p.t)));
  const yMax = niceMax(Math.max(0, ...modes.flatMap(m => m.points.map(p => p[chart.key]))) * 1.08);
  const x = t => L + (t / tMax) * (W - L - R), y = v => T + (1 - v / yMax) * (H - T - B);
  let s = "";
  for (let i = 0; i <= 4; i++) {
    const v = yMax * i / 4, yy = y(v);
    s += `<line x1="${L}" x2="${W - R}" y1="${yy}" y2="${yy}" stroke="${css("--grid")}" stroke-width="1"/>`;
    s += `<text x="${L - 6}" y="${yy + 4}" text-anchor="end">${chart.fmt(v)}</text>`;
  }
  const step = tMax > 30 ? 10 : tMax > 12 ? 5 : 2;
  for (let t = 0; t <= tMax + 1e-9; t += step) {
    s += `<text x="${x(t)}" y="${H - 6}" text-anchor="middle">${t}s</text>`;
  }
  modes.forEach((m, i) => {
    if (!m.points.length) return;
    const col = css(COLORS[i % 4]);
    const d = m.points.map((p, j) => `${j ? "L" : "M"}${x(p.t).toFixed(1)},${y(p[chart.key]).toFixed(1)}`).join("");
    s += `<path d="${d}" fill="none" stroke="${col}" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"/>`;
    const last = m.points[m.points.length - 1];
    s += `<circle cx="${x(last.t)}" cy="${y(last[chart.key])}" r="4" fill="${col}" stroke="${css("--card")}" stroke-width="2"/>`;
  });
  s += `<line class="cross" x1="0" x2="0" y1="${T}" y2="${H - B}" stroke="${css("--muted")}" stroke-width="1" stroke-dasharray="3 3" visibility="hidden"/>`;
  s += `<rect x="${L}" y="${T}" width="${W - L - R}" height="${H - T - B}" fill="transparent"/>`;
  svg.setAttribute("viewBox", `0 0 ${W} ${H}`);
  svg.innerHTML = s;
  chart.geom = {x, tMax, L, R, W};
}

function hover(chart, ev) {
  const svg = document.getElementById(chart.id), tip = svg.nextElementSibling, g = chart.geom;
  if (!state || !g) return;
  const box = svg.getBoundingClientRect(), px = (ev.clientX - box.left) * (g.W / box.width);
  const t = Math.max(0, Math.min(g.tMax, (px - g.L) / (g.W - g.L - g.R) * g.tMax));
  let rows = "";
  state.modes.forEach((m, i) => {
    if (!m.points.length) return;
    const p = m.points.reduce((a, b) => Math.abs(b.t - t) < Math.abs(a.t - t) ? b : a);
    rows += `<div><i style="background:${css(COLORS[i % 4])}"></i>${m.label}: <b>${chart.fmt(p[chart.key])}${chart.unit}</b> <span style="color:${css("--muted")}">@${p.t.toFixed(1)}s</span></div>`;
  });
  const cross = svg.querySelector(".cross");
  if (!rows) { tip.style.display = "none"; return; }
  cross.setAttribute("x1", g.x(t)); cross.setAttribute("x2", g.x(t)); cross.setAttribute("visibility", "visible");
  tip.innerHTML = rows; tip.style.display = "block";
  const card = svg.parentElement.getBoundingClientRect();
  let left = ev.clientX - card.left + 14;
  if (left + tip.offsetWidth > card.width) left = ev.clientX - card.left - tip.offsetWidth - 14;
  tip.style.left = Math.max(4, left) + "px"; tip.style.top = (ev.clientY - card.top + 10) + "px";
}

const median = a => { if (!a.length) return null; const s = [...a].sort((p, q) => p - q), h = s.length >> 1; return s.length % 2 ? s[h] : (s[h - 1] + s[h]) / 2; };
const check = (v, st) => v === null ? `<span class="pend">${st === "checking" ? "checking..." : "-"}</span>` : v ? `<span class="ok">✓ yes</span>` : `<span class="bad">✗ NO</span>`;

function render() {
  if (!state) return;
  const c = state.config;
  document.getElementById("config").textContent =
    `${c.clients} client processes × ${fmtN(c.requests_per_client)} requests per round, window ${c.max_inflight}, queue ${c.queue_capacity}, ${c.seconds}s per mode`;
  document.getElementById("status").textContent = state.message;
  document.getElementById("start").disabled = state.status === "running";
  document.getElementById("start").textContent = state.status === "waiting" ? "Start" : "Run again";
  document.getElementById("legend").innerHTML = state.modes.map((m, i) =>
    `<span><i style="background:${css(COLORS[i % 4])}"></i>${m.label}<em>${m.status === "running" ? "running" : m.status === "pending" ? "waiting" : m.status === "checking" ? "checking" : ""}</em></span>`).join("");
  document.getElementById("rows").innerHTML = state.modes.map((m, i) => {
    const med = k => median(m.points.map(p => p[k]));
    const v = (k, f, u) => med(k) === null ? `<span class="pend">-</span>` : f(med(k)) + u;
    return `<tr><td><span class="sw" style="background:${css(COLORS[i % 4])}"></span>${m.label}</td>
      <td>${v("rps", fmtN, " req/s")}</td><td>${v("avg", fmtN, " µs")}</td><td>${v("p95", fmtN, " µs")}</td>
      <td>${fmtN(m.requests)}</td><td>${fmtN(m.accepted)} / ${fmtN(m.rejected)}</td>
      <td>${check(m.invariants, m.status)}</td><td>${check(m.oracle, m.status)}</td></tr>`;
  }).join("");
  CHARTS.forEach(draw);
}

async function poll() {
  try {
    state = await (await fetch("/state", {cache: "no-store"})).json();
    render();
  } catch (e) {
    document.getElementById("status").textContent = "Lost connection to the script.";
  }
  setTimeout(poll, 500);
}
document.getElementById("start").onclick = async () => { await fetch("/start", {method: "POST"}); };
CHARTS.forEach(ch => {
  const svg = document.getElementById(ch.id);
  svg.addEventListener("mousemove", ev => hover(ch, ev));
  svg.addEventListener("mouseleave", () => { svg.nextElementSibling.style.display = "none";
    const c = svg.querySelector(".cross"); if (c) c.setAttribute("visibility", "hidden"); });
});
window.addEventListener("resize", () => CHARTS.forEach(draw));
matchMedia("(prefers-color-scheme: dark)").addEventListener("change", render);
poll();
</script>
</body></html>
"""

if __name__ == "__main__":
    main()

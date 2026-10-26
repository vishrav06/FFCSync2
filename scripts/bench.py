#!/usr/bin/env python3
"""Small, reproducible FFCSync benchmark.

For each workload size, the workload is generated deterministically and
split across --clients client processes. Configurations measured:

  * sequential_engine : ffcsync_seq run (in-process, no IPC) - the reference
  * server, workers=W : ffcsync_server --workers W (W=0 is the sequential server)
  * server, committer : ffcsync_server --mode committer (one batched committer thread)

Every server run is checked for correctness BEFORE its numbers are kept:
the server's admission log is replayed through ffcsync_seq and the results
and final state must be byte-identical, and the server's invariants must
pass. Output: results/bench/metrics.csv, summary.md, environment.txt.

    python3 scripts/bench.py                      # default matrix
    python3 scripts/bench.py --sizes 1000 --workers 0 1 2 4 --reps 3
"""
import argparse
import csv
import filecmp
import os
import platform
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)


def generate(n, clients, seed, tmp):
    files = []
    per = n // clients
    for c in range(clients):
        f = tmp / f"w{n}_c{c}.csv"
        run([sys.executable, "-I", str(ROOT / "scripts/gen_workload.py"), "--data", str(ROOT / "data"),
             "--n", str(per), "--seed", str(seed * 100 + c), "--prefix", chr(ord("A") + c),
             "--out", str(f)])
        files.append(f)
    combined = tmp / f"w{n}_all.csv"
    with combined.open("w") as out:
        out.write("request_id,op,student_id,section_id\n")
        for f in files:
            out.writelines(f.read_text().splitlines(keepends=True)[1:])
    return files, combined


def percentile(sorted_vals, p):
    if not sorted_vals:
        return 0
    k = max(0, -(-len(sorted_vals) * p // 100) - 1)  # nearest-rank
    return sorted_vals[int(k)]


def bench_sequential(build, combined, tmp):
    out = tmp / "seq"
    t0 = time.perf_counter()
    r = run([str(build / "ffcsync_seq"), "run", "--data", str(ROOT / "data"), "--requests", str(combined),
             "--out", str(out)])
    wall_ms = (time.perf_counter() - t0) * 1000
    info = dict(line.split(": ", 1) for line in r.stdout.splitlines() if ": " in line and not line.startswith(" "))
    proc_ms = float(info["processing_ms"])
    n = int(info["requests"])
    return {
        "wall_ms": round(proc_ms, 3), "process_wall_ms": round(wall_ms, 3),
        "throughput_rps": round(n / (proc_ms / 1000), 1) if proc_ms > 0 else 0,
        "avg_latency_us": "", "p95_latency_us": "", "avg_server_us": "",
        "accepted": int(info["accepted"]), "rejected": int(info["rejected"]),
        "invariants_ok": int(info["invariants"] == "OK"), "matches_oracle": "reference",
    }


def bench_server(build, workers, queue, client_files, tmp):
    committer = workers == "committer"
    sock = tmp / "bench.sock"
    srv_out = tmp / f"srv_w{workers}"
    shutil.rmtree(srv_out, ignore_errors=True)
    server = subprocess.Popen([str(build / "ffcsync_server"), "--data", str(ROOT / "data"),
                               "--socket", str(sock), "--workers", "1" if committer else str(workers),
                               "--mode", "committer" if committer else "gate",
                               "--queue-capacity", str(queue), "--out", str(srv_out)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(200):
            if sock.exists():
                break
            time.sleep(0.01)
        else:
            raise RuntimeError("server did not start")

        outs = [tmp / f"client{i}.out" for i in range(len(client_files))]
        t0 = time.perf_counter()
        clients = [subprocess.Popen([str(build / "ffcsync_client"), "--socket", str(sock), "--requests", str(f),
                                     "--quiet", "--out", str(o)], stderr=subprocess.DEVNULL)
                   for f, o in zip(client_files, outs)]
        codes = [c.wait() for c in clients]
        wall_ms = (time.perf_counter() - t0) * 1000
        if any(codes):
            raise RuntimeError(f"client failed: {codes}")
    finally:
        server.send_signal(signal.SIGTERM)
        server_code = server.wait()

    oracle = tmp / "oracle"
    run([str(build / "ffcsync_seq"), "replay", "--data", str(ROOT / "data"),
         "--admission-log", str(srv_out / "admission.csv"), "--out", str(oracle)])
    same = all(filecmp.cmp(srv_out / f, oracle / f, shallow=False) for f in ("results.csv", "final_state.csv"))
    inv_ok = server_code == 0 and "invariants: OK" in (srv_out / "summary.txt").read_text()

    lat, srv_us, accepted, total = [], [], 0, 0
    for o in outs:
        with o.open() as f:
            for row in csv.DictReader(f):
                total += 1
                lat.append(int(row["latency_us"]))
                srv_us.append(int(row["server_us"]))
                accepted += row["status"] == "ACCEPTED"
    lat.sort()
    return {
        "wall_ms": round(wall_ms, 3), "process_wall_ms": round(wall_ms, 3),
        "throughput_rps": round(total / (wall_ms / 1000), 1),
        "avg_latency_us": round(statistics.fmean(lat), 1), "p95_latency_us": percentile(lat, 95),
        "avg_server_us": round(statistics.fmean(srv_us), 2),
        "accepted": accepted, "rejected": total - accepted,
        "invariants_ok": int(inv_ok), "matches_oracle": int(same),
    }


def environment():
    lines = [f"date: {time.strftime('%Y-%m-%d %H:%M:%S %Z')}", f"platform: {platform.platform()}",
             f"python: {platform.python_version()}", f"cpus: {os.cpu_count()}"]
    try:
        model = next(l.split(":", 1)[1].strip() for l in Path("/proc/cpuinfo").read_text().splitlines()
                     if l.startswith("model name"))
        lines.append(f"cpu: {model}")
    except (OSError, StopIteration):
        pass
    try:
        lines.append("compiler: " + run(["c++", "--version"]).stdout.splitlines()[0])
    except (OSError, subprocess.CalledProcessError):
        pass
    return "\n".join(lines) + "\n"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--build", default=str(ROOT / "build"))
    p.add_argument("--sizes", type=int, nargs="+", default=[1000, 10000])
    p.add_argument("--workers", type=int, nargs="+", default=[0, 1, 2, 4])
    p.add_argument("--committer", action=argparse.BooleanOptionalAction, default=True,
                   help="also measure --mode committer (shown as workers=committer)")
    p.add_argument("--clients", type=int, default=4)
    p.add_argument("--queue-capacity", type=int, default=64)
    p.add_argument("--reps", type=int, default=3)
    p.add_argument("--seed", type=int, default=2026)
    p.add_argument("--out", default=str(ROOT / "results/bench"))
    a = p.parse_args()

    build, out = Path(a.build), Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "environment.txt").write_text(environment())
    fields = ["mode", "workers", "requests", "clients", "rep", "wall_ms", "process_wall_ms", "throughput_rps",
              "avg_latency_us", "p95_latency_us", "avg_server_us", "accepted", "rejected",
              "invariants_ok", "matches_oracle"]
    rows = []
    with tempfile.TemporaryDirectory(prefix="ffcsync_bench_") as t:
        tmp = Path(t)
        for n in a.sizes:
            client_files, combined = generate(n, a.clients, a.seed + n, tmp)
            for rep in range(1, a.reps + 1):
                configs = [("sequential_engine", "")] + [("server", w) for w in a.workers]
                if a.committer:
                    configs.append(("server", "committer"))
                for mode, w in configs:
                    m = (bench_sequential(build, combined, tmp) if mode == "sequential_engine"
                         else bench_server(build, w, a.queue_capacity, client_files, tmp))
                    row = {"mode": mode, "workers": w, "requests": n, "clients": a.clients if mode == "server" else 1,
                           "rep": rep, **m}
                    rows.append(row)
                    print(f"n={n:6d} rep={rep} {mode:17s} workers={str(w):9s} "
                          f"throughput={row['throughput_rps']:>10} rps  p95={row['p95_latency_us']!s:>6} us  "
                          f"correct={row['matches_oracle']} invariants={row['invariants_ok']}", flush=True)

    with (out / "metrics.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, lineterminator="\n")
        w.writeheader()
        w.writerows(rows)

    # Markdown summary: median over repetitions.
    md = ["| requests | mode | workers | median throughput (req/s) | median avg latency (us) | "
          "median p95 latency (us) | correct (all reps) |", "|---|---|---|---|---|---|---|"]
    keys = sorted({(r["requests"], r["mode"], str(r["workers"])) for r in rows},
                  key=lambda k: (k[0], k[1] != "sequential_engine", k[2]))
    all_ok = True
    for n, mode, w in keys:
        g = [r for r in rows if (r["requests"], r["mode"], str(r["workers"])) == (n, mode, w)]
        ok = all(r["invariants_ok"] == 1 and r["matches_oracle"] in (1, "reference") for r in g)
        all_ok &= ok
        med = lambda k: (statistics.median(float(r[k]) for r in g) if g[0][k] != "" else None)
        fmt = lambda v: "n/a" if v is None else f"{v:,.0f}"
        md.append(f"| {n} | {mode} | {w or '-'} | {fmt(med('throughput_rps'))} | {fmt(med('avg_latency_us'))} | "
                  f"{fmt(med('p95_latency_us'))} | {'yes' if ok else 'NO'} |")
    (out / "summary.md").write_text("\n".join(md) + "\n")
    print("\n".join(md))
    print(f"\nwrote {out/'metrics.csv'}, {out/'summary.md'}, {out/'environment.txt'}")
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()

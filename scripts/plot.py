#!/usr/bin/env python3
"""Plot benchmark results produced by scripts/bench.py (requires matplotlib).

    .venv/bin/python scripts/plot.py [--metrics results/bench/metrics.csv]

Writes throughput.png and latency.png next to the metrics file. Each bar is
the median over repetitions; runs that failed the correctness check are
refused rather than plotted.
"""
import argparse
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--metrics", default=str(Path(__file__).resolve().parent.parent / "results/bench/metrics.csv"))
    a = p.parse_args()
    metrics = Path(a.metrics)
    rows = list(csv.DictReader(metrics.open()))

    bad = [r for r in rows if r["invariants_ok"] != "1" or r["matches_oracle"] not in ("1", "reference")]
    if bad:
        sys.exit(f"refusing to plot: {len(bad)} run(s) failed correctness checks")

    groups = defaultdict(list)
    for r in rows:
        if r["mode"] == "server":
            groups[(int(r["requests"]), r["workers"])].append(r)
    sizes = sorted({k[0] for k in groups})
    # Numeric worker counts first, then the committer mode.
    workers = sorted({k[1] for k in groups}, key=lambda w: (not w.isdigit(), int(w) if w.isdigit() else 0))

    def label(w):
        if w == "committer":
            return "committer"
        return "sequential server" if w == "0" else f"{w} worker" + ("s" if int(w) > 1 else "")

    labels = [label(w) for w in workers]
    width = 0.8 / len(sizes)

    def bar_chart(field, ylabel, title, filename):
        fig, ax = plt.subplots(figsize=(7, 4))
        for i, n in enumerate(sizes):
            vals = [statistics.median(float(r[field]) for r in groups[(n, w)]) for w in workers]
            xs = [x + (i - (len(sizes) - 1) / 2) * width for x in range(len(workers))]
            bars = ax.bar(xs, vals, width, label=f"{n:,} requests")
            ax.bar_label(bars, fmt="{:,.0f}", fontsize=7, padding=2)
        ax.set_xticks(range(len(workers)), labels)
        ax.set_ylabel(ylabel)
        ax.set_title(title)
        ax.legend(frameon=False)
        ax.spines[["top", "right"]].set_visible(False)
        fig.tight_layout()
        fig.savefig(metrics.parent / filename, dpi=150)
        print(f"wrote {metrics.parent / filename}")

    bar_chart("throughput_rps", "requests / second (median)", "FFCSync server throughput", "throughput.png")
    bar_chart("p95_latency_us", "p95 latency, microseconds (median)", "FFCSync client-observed p95 latency",
              "latency.png")


if __name__ == "__main__":
    main()

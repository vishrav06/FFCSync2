#!/usr/bin/env python3
"""Deterministic workload generator for FFCSync.

Writes a request CSV (request_id,op,student_id,section_id) using the
students and sections of a catalog directory. The same --seed always
produces the same file, so benchmark runs are repeatable.

    python3 scripts/gen_workload.py --n 1000 --seed 1 --out results/w1000.csv
    python3 scripts/gen_workload.py --n 500 --seed 2 --prefix B --out b.csv
"""
import argparse
import csv
import pathlib
import random


def ids(path: pathlib.Path) -> list[str]:
    with path.open(newline="") as f:
        rows = [r for r in csv.reader(f) if r and not r[0].startswith("#")]
    return [r[0].strip() for r in rows[1:]]


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--data", default="data", help="catalog directory")
    p.add_argument("--n", type=int, required=True, help="number of requests")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--drop-ratio", type=float, default=0.25, help="fraction of DROP requests")
    p.add_argument("--hot-ratio", type=float, default=0.5,
                   help="fraction of ADDs aimed at the single-seat 'hot' sections")
    p.add_argument("--prefix", default="W", help="request_id prefix (keep unique per client)")
    p.add_argument("--out", required=True)
    a = p.parse_args()

    data = pathlib.Path(a.data)
    students = ids(data / "students.csv")
    with (data / "sections.csv").open(newline="") as f:
        rows = [r for r in csv.reader(f) if r and not r[0].startswith("#")][1:]
    sections = [r[0].strip() for r in rows]
    hot = [r[0].strip() for r in rows if int(r[4]) <= 1] or sections

    rng = random.Random(a.seed)
    out = pathlib.Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["request_id", "op", "student_id", "section_id"])
        for i in range(a.n):
            op = "DROP" if rng.random() < a.drop_ratio else "ADD"
            pool = hot if op == "ADD" and rng.random() < a.hot_ratio else sections
            w.writerow([f"{a.prefix}{i:06d}", op, rng.choice(students), rng.choice(pool)])


if __name__ == "__main__":
    main()

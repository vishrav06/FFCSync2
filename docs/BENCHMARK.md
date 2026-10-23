# FFCSync — Benchmark

## Reproduce

```bash
cmake -S . -B build && cmake --build build -j
python3 scripts/bench.py                         # writes results/bench/{metrics.csv,summary.md,environment.txt}
python3 -m venv .venv && .venv/bin/pip install matplotlib   # once
.venv/bin/python scripts/plot.py                 # writes results/bench/{throughput,latency}.png
```

Options: `--sizes 1000 10000 --workers 0 1 2 4 --clients 4 --reps 3 --queue-capacity 64 --seed 2026`.

## What is measured

For each workload size, `gen_workload.py` generates the workload deterministically and splits it into 4 files, one per client process. Each repetition then runs:

| Mode | What is timed |
|---|---|
| `sequential_engine` | `ffcsync_seq run` on the combined file. This is engine time only: no IPC, no threads (`processing_ms` reported by the program). |
| `server`, workers = 0 | Inline sequential server. Wall time from launching the 4 client processes until all have received every response. |
| `server`, workers = 1/2/4 | Queue + worker pool + commit gate. Same timing. |

Per row of `metrics.csv`: wall time, throughput, average and p95 client-observed latency (nearest-rank, over all requests of all clients), average `server_us`, accepted and rejected counts, and two correctness columns.

**Correctness is checked for every server run before its numbers are kept.**
- `invariants_ok`: the server's own invariant check passed and it exited with code 0.
- `matches_oracle`: `ffcsync_seq replay` of the server's admission log produced byte-identical `results.csv` and `final_state.csv`.

`bench.py` exits non-zero, and `plot.py` refuses to plot, if any run fails.

## Results (one machine, 2026-10-23)

Environment (`results/bench/environment.txt`): WSL2 on Linux 6.6.87, AMD Ryzen 5 4500U (6 CPUs), g++ 15.2 `RelWithDebInfo`, 4 client processes, queue capacity 64, 3 repetitions. The table shows medians.

| requests | mode | workers | throughput (req/s) | avg latency (µs) | p95 latency (µs) | correct (all reps) |
|---|---|---|---|---|---|---|
| 1000 | sequential_engine | - | 1,200,982 | n/a | n/a | yes |
| 1000 | server | 0 | 93,462 | 1,167 | 2,076 | yes |
| 1000 | server | 1 | 94,286 | 1,084 | 2,480 | yes |
| 1000 | server | 2 | 82,870 | 1,554 | 2,826 | yes |
| 1000 | server | 4 | 51,160 | 3,242 | 7,045 | yes |
| 10000 | sequential_engine | - | 1,133,522 | n/a | n/a | yes |
| 10000 | server | 0 | 154,414 | 1,304 | 3,197 | yes |
| 10000 | server | 1 | 149,978 | 1,371 | 3,269 | yes |
| 10000 | server | 2 | 122,729 | 1,731 | 3,839 | yes |
| 10000 | server | 4 | 59,266 | 3,986 | 8,246 | yes |

![throughput](../results/bench/throughput.png)

## Interpretation (limited to these measurements)

- **Every configuration was correct in every repetition.** No overselling, no invariant violation, and results identical to the sequential replay.
- **Adding workers made the server slower on this workload.** The registration rules take well under a microsecond per request (the in-process engine handles more than a million requests per second). So the cost is dominated by socket I/O, thread hand-offs and the serialized commit gate.
  - With more workers, more threads wait at the gate. Each commit's `notify_all` wakes all of them, and only one can proceed.
  - This is the expected price of the strict-FCFS ordered commit design described in `ARCHITECTURE.md` §4.
- The server reaches ~10 % of the bare engine's throughput. The difference is the cost of IPC, framing and process/thread boundaries.
- These numbers come from one laptop under WSL2 and short runs, and they vary between runs. They support only the relative conclusions above, not absolute capacity claims.

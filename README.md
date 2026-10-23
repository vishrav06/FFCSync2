# FFCSync

An Operating Systems teaching project that simulates VIT-style FFCS course registration during a high-demand registration window. A **client process** and a **registration-server process** communicate over a **Unix domain socket**. The server pushes requests through a **bounded FIFO queue** to a **worker-thread pool**, and commits them in strict **FCFS order** through an ordered commit gate. A **sequential reference engine** proves that the concurrent results are correct.

This is a simulation, not a production registration system. See [docs/SCOPE.md](docs/SCOPE.md) for the rules, the FCFS definition and the non-goals.

| Document | Contents |
|---|---|
| [docs/EXPLAINER.md](docs/EXPLAINER.md) | Start here: architecture walkthrough, OS concepts, and the purpose of every file |
| [docs/SCOPE.md](docs/SCOPE.md) | Rules and rejection reasons, FCFS definition, data formats, non-goals |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Process/thread diagram, synchronization, correctness argument |
| [docs/IPC_PROTOCOL.md](docs/IPC_PROTOCOL.md) | Socket, framing and message format |
| [docs/TESTING.md](docs/TESTING.md) | Test strategy and commands |
| [docs/BENCHMARK.md](docs/BENCHMARK.md) | Benchmark method, results and interpretation |

## Setup (Linux or WSL2 Ubuntu)

```bash
sudo apt install build-essential cmake python3 python3-venv   # g++ with C++20, CMake >= 3.20
```

No other dependencies are needed. The test framework (doctest 2.4.11) is vendored in `third_party/`. matplotlib is only needed for the optional plotting script.

## Build and test

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run

```bash
# Sequential reference engine on a request file
./build/ffcsync_seq run --data data --requests data/workloads/basic.csv --out results/seq

# Server (Ctrl-C or SIGTERM for graceful shutdown); --workers 0 = sequential server
./build/ffcsync_server --data data --socket /tmp/ffcsync.sock --workers 4 --queue-capacity 64 \
                       --out results/server --verbose

# Clients (in another terminal)
./build/ffcsync_client --op ADD --student S005 --section HUM1021-A2        # one request
./build/ffcsync_client --raw "REQ|x|JOIN|S001|CSE2005-A1"                   # malformed message
./build/ffcsync_client --requests data/workloads/basic.csv --out results/client.csv

# After stopping the server: verify it against the sequential reference
./build/ffcsync_seq replay --data data --admission-log results/server/admission.csv --out results/oracle
diff results/server/results.csv results/oracle/results.csv
```

## Demonstration

```bash
scripts/demo.sh
```

The demo shows one accepted request, one rule violation, one malformed message, and **two client processes racing for the last seat** of `CSE2005-B1`. It then shuts down gracefully, checks the invariants, and compares the server's output with the sequential replay. Outputs go to `results/demo/`.

## Benchmark

```bash
python3 scripts/bench.py                                      # results/bench/metrics.csv + summary.md
python3 -m venv .venv && .venv/bin/pip install matplotlib     # once
.venv/bin/python scripts/plot.py                              # results/bench/*.png
```

## Layout

```text
include/ffcsync/   public headers (model, rules, engines, queue, gate, IPC, server)
src/               library implementation (ffcsync_core)
apps/              ffcsync_seq, ffcsync_server, ffcsync_client
tests/             doctest suites + two-process integration script
data/              example catalog and workloads (+ golden expected outputs)
scripts/           workload generator, benchmark, plots, demo
docs/              documentation
results/           generated output (not hand-edited, git-ignored)
```

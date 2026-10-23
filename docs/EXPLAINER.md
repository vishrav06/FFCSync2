# FFCSync Explained

This explains how FFCSync works and what every file in the repository is for. It assumes OS-course knowledge (processes, threads, mutexes, condition variables, sockets) but no familiarity with the code.

---

## 1. The problem in one paragraph

When FFCS registration opens, thousands of students try to grab the same few seats at the same instant. If two requests read "1 seat left" at the same time and both write "registered", the section is oversold. That is a classic **race condition** on shared state.

FFCSync rebuilds this situation in miniature and shows how standard OS mechanisms prevent it. The mechanisms are **process isolation**, **IPC**, a **bounded producer/consumer queue**, a **thread pool**, and an **ordered critical section**. They also enforce a precise fairness rule, **first come, first served (FCFS)**.

## 2. The big picture

```text
  client process(es)                       server process (owns ALL mutable state)
 ┌──────────────────┐   Unix socket   ┌──────────────────────────────────────────────┐
 │ ffcsync_client   │ ══ frames ════► │ reader thread ─► ADMISSION (seq = 1,2,3…)    │
 │  sender thread   │                 │                    │                         │
 │  receiver (main) │ ◄═ responses ══ │             bounded FIFO queue               │
 └──────────────────┘                 │                    │                         │
                                      │   worker threads: static checks (parallel)   │
                                      │                    │                         │
                                      │   commit gate: one at a time, in seq order   │
                                      │     re-check seats/clash/credits → commit    │
                                      └──────────────────────────────────────────────┘
  ffcsync_seq: the same rules, single-threaded. It replays the server's admission
               order and must produce identical output (the "oracle").
```

There are three executables:

| Executable | Role |
|---|---|
| `ffcsync_server` | Long-running server process. It loads the catalog, listens on a Unix socket, and owns and changes the registration state. |
| `ffcsync_client` | Client and load generator. It sends one request, a raw message, or a whole workload file, and records responses and latency. It never touches state. |
| `ffcsync_seq` | The sequential reference engine. `run` processes a request file in order, and `replay` processes a server's admission log in seq order. |

## 3. Where each OS concept appears

| OS concept | Where | What to point at |
|---|---|---|
| **Processes and isolation** | `ffcsync_server` vs `ffcsync_client` | Clients cannot corrupt server memory. The only way to change state is to send a message. |
| **IPC (Unix domain socket)** | `src/ipc_protocol.cpp`, `src/server.cpp` | `socket/bind/listen/accept/connect`, a socket file in `/tmp`, unlinked on shutdown |
| **Message framing** | `read_frame` / `write_frame` | A stream has no message boundaries. A 4-byte length prefix and full-read/full-write loops handle partial I/O. |
| **Threads** | reader thread per connection, N workers, main accept thread; client sender + receiver | `std::thread`, always joined and never detached |
| **Mutual exclusion** | `admission_mu_`, the queue mutex, the gate mutex, the per-connection `write_mu` | Each one guards a clearly named piece of data (`ARCHITECTURE.md` §1 table) |
| **Condition variables** | `BoundedQueue` (`not_full`, `not_empty`), `CommitGate` (`turn_`) | Sleeping instead of busy-waiting; predicate loops against spurious wake-ups |
| **Bounded buffer / producer–consumer** | `include/ffcsync/request_queue.hpp` | Readers produce and workers consume. A full queue blocks producers (back-pressure). |
| **Ordering vs exclusion** | `include/ffcsync/commit_gate.hpp` | A mutex alone does not give FIFO; a ticket counter (`next_`) does |
| **Race conditions / TOCTOU** | `check_static` vs `check_mutable` | Facts that can change (seats, timetable) are re-checked *inside* the critical section |
| **Deadlock avoidance** | gate + FIFO queue; fixed lock order | Jobs are popped in seq order, so every earlier seq is already held by a running worker |
| **Semaphores** | client `std::counting_semaphore` | Limits the number of in-flight requests (a sliding window) |
| **Signals** | `apps/server_main.cpp` | The SIGINT/SIGTERM handler writes one byte to a self-pipe (async-signal-safe), which wakes `poll()` |
| **Graceful shutdown** | `Server::run()` tail | Stop accepting → `SHUT_RD` readers → join → close queue → drain workers → join → write outputs |

## 4. Life of a request, step by step

Take request `R07: ADD S002 → CSE2005-B1` (a 1-seat section).

1. **Client.** The sender thread waits for a free slot on the semaphore window. It then encodes `REQ|R07|ADD|S002|CSE2005-B1` and writes `[len][payload]` to the socket.
2. **Reader thread** (`Server::reader_loop`). It reads one frame and decodes it. If the message were malformed, the reader would reply `MALFORMED` with `seq 0` and the request would never enter the system.
3. **Admission** (`ConcurrentEngine::submit`). Under `admission_mu_`, the request gets `seq = 7`, is appended to the admission log, and is pushed onto the queue. **This is the FCFS moment:** whatever was admitted earlier has a smaller seq and wins conflicts. A full queue blocks here.
4. **Worker, phase A** (`check_static`). Is the window open? Do the student and section exist? Has S002 completed CSE2001? These checks need no lock, because the catalog never changes.
5. **Worker, phase B** (`CommitGate::run_in_order(7, …)`). The worker sleeps until seq 1–6 have committed. Then, alone, it runs `check_mutable`: already registered? same course? **seat still free?** clash? credits? bucket? If everything passes, `apply_change` takes the seat. It increments the turn counter and wakes the waiters.
6. **Worker, phase C.** It sends `RES|R07|7|ACCEPTED|OK|38` back on S002's socket.
7. **Client.** The receiver thread matches `R07`, records the latency and frees a semaphore slot.

Suppose S006 also asked for CSE2005-B1 and was admitted as seq 8. Its worker might finish phase A *before* seq 7's worker. It still has to wait at the gate. When its turn comes, the seat is gone, so it gets `SECTION_FULL`. That is exactly the result the sequential engine gives for the order 7 → 8.

## 5. Why we can trust the result

- **Same code path.** Both engines commit through `decide_and_commit()` in `src/rules.cpp`. The only difference is *who decides the order*: file order (sequential) or admission order (server).
- **The oracle test.** After any concurrent run, replaying the admission log through `RegistrationEngine` must reproduce every result and the final state byte for byte. This is checked:
  - thousands of times in `tests/test_concurrency.cpp`
  - across real processes in `tests/integration.sh`
  - in every benchmark run in `scripts/bench.py`
- **Invariants.** These are checked after every run: no section over capacity, seat counters match registrations, credits and buckets recompute correctly and stay within limits, no timetable clashes, prerequisites satisfied.
- **ThreadSanitizer.** `-DFFCSYNC_SANITIZE=thread` runs the whole suite with no reported races.

## 6. The trade-off, measured

Commits are serialized on purpose, because it makes FCFS easy to prove. The benchmark (`docs/BENCHMARK.md`) shows the cost honestly. On this workload one worker matches the inline sequential server, and 2 or 4 workers are *slower*. Per-request rule checking is tiny, so the extra thread hand-offs at the gate dominate.

The project's claim is **correctness under concurrency, not speed-up**. A faster design would order only *conflicting* requests (same student or same section). It is deliberately out of scope.

## 7. Every file and what it is for

### Build and configuration

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Builds the `ffcsync_core` library, the three executables and `ffcsync_tests`. Registers the CTest tests. The `FFCSYNC_SANITIZE` option enables TSan/ASan. |
| `.gitignore` | Ignores build directories, the Python venv and generated `results/`. |
| `README.md` | Setup, build, run, test, demo and benchmark commands. |

### Public headers: `include/ffcsync/`

| File | Purpose |
|---|---|
| `model.hpp` | All core types: `Student`, `Course`, `Section`, `Config`, `Catalog` (immutable), `RegistrationRequest`, `RegistrationResult`, `StudentState`, `RegistrationState` (mutable), plus the `Op`/`Status`/`Reason` enums and their string conversions. |
| `dataset.hpp` | CSV reading (`read_csv`, `split`, `trim`), `load_catalog(dir)`, `load_requests(file)`, and the `DataError` exception. |
| `rules.hpp` | The registration rules, split into `check_static` (catalog only, lock-free) and `check_mutable` (state, commit time). Also `apply_change` and the shared commit step `decide_and_commit`. |
| `registration_engine.hpp` | `RegistrationEngine`, the single-threaded reference engine (`apply`, `apply_with_seq`, `run`). |
| `invariants.hpp` | `check_invariants(catalog, state)`: recomputes and validates the whole state and returns a list of violations. |
| `result_io.hpp` | Writers and readers for `results.csv`, `admission.csv` and `final_state.csv`, the summary printer, and the `write_file` helper. |
| `request_queue.hpp` | `BoundedQueue<T>`: a blocking bounded FIFO with one mutex and the `not_full`/`not_empty` condition variables, plus `close()`. Header-only template. |
| `commit_gate.hpp` | `CommitGate`: runs a function under a mutex **in seq order** (ticket counter + condition variable). The header comment explains why a mutex alone is insufficient and why it cannot deadlock. |
| `concurrent_engine.hpp` | `ConcurrentEngine`: the admission point (`submit`), the worker pool, the queue and gate wiring, `shutdown`, and the state/results/admission snapshots. `workers = 0` selects inline sequential processing. |
| `ipc_protocol.hpp` | Framing (`read_frame`, `write_frame`), the request/response codec, and the `listen_unix`/`connect_unix` socket helpers. |
| `server.hpp` | `Server`: socket lifecycle, the accept loop, reader threads, the async-signal-safe `request_stop`, graceful shutdown and output files. |

### Implementation: `src/`

| File | Purpose |
|---|---|
| `model.cpp` | Enum ↔ string tables, catalog lookups, `RegistrationState::empty_for`. |
| `dataset.cpp` | CSV parsing with header validation, integer parsing, cross-reference checks (unknown course, prerequisite, duplicates). |
| `rules.cpp` | The rule implementations in the documented order, and the exact ADD/DROP state updates. |
| `registration_engine.cpp` | The sequential engine: assigns seqs in order and calls `check_static` + `decide_and_commit`. |
| `invariants.cpp` | The invariant checker. |
| `result_io.cpp` | Deterministic CSV writers and readers, and the summary. |
| `concurrent_engine.cpp` | Admission (seq + log + push under `admission_mu_`), the worker loop (phases A/B/C), verbose logging, and orderly shutdown. |
| `ipc_protocol.cpp` | Full-read/full-write loops (EINTR-safe, `MSG_NOSIGNAL`), frame encode/decode, message validation, socket creation. |
| `server.cpp` | Stale-socket detection, the `poll` loop on the listening socket plus the self-pipe, `accept4`, the per-connection reader thread, the per-connection write mutex, the shutdown sequence and output writing. |

### Executables: `apps/`

| File | Purpose |
|---|---|
| `cli.hpp` | A tiny `--key value` / `--flag` argument parser shared by the three programs. |
| `seq_main.cpp` | `ffcsync_seq run`/`replay`: writes results and final state, prints the summary and `processing_ms`, runs the invariant check, and exits 1 on violation. |
| `server_main.cpp` | `ffcsync_server`: parses options, installs the SIGINT/SIGTERM handlers (self-pipe) and ignores SIGPIPE, runs the server, and exits 1 on an invariant violation. |
| `client_main.cpp` | `ffcsync_client`: single-request, raw-message and pipelined workload modes. In workload mode the sender thread is throttled by a `counting_semaphore` while the main thread receives responses. Writes the per-request CSV and appends to a metrics CSV. |

### Tests: `tests/`

| File | Purpose |
|---|---|
| `test_main.cpp` | The doctest `main()`. |
| `fixtures.hpp` | Shared helpers: the example catalog, `add()`/`drop()` request builders, `read_text`. |
| `server_harness.hpp` | `TestServer` (runs a `Server` in a background thread on a unique socket path) and `TestClient` (blocking request/response). |
| `test_rules.cpp` | One test per registration rule plus seq behaviour. |
| `test_invariants.cpp` | The checker accepts valid states and catches oversold sections, counter mismatches, wrong credits or buckets, and clashes. |
| `test_golden.cpp` | Golden workloads vs the stored expected outputs, determinism, and CSV round-trips. |
| `test_ipc.cpp` | Partial I/O, oversized/truncated frames, the codec, server-vs-oracle, MALFORMED, disconnects, the socket file lifecycle. |
| `test_queue.cpp` | FIFO order, capacity under contention, producer blocking, `close()` semantics, engine shutdown and drain. |
| `test_concurrency.cpp` | Last-seat races, same-student conflicts, and randomized oracle equivalence for 0/1/2/4/8 workers. |
| `integration.sh` | The real server plus 3 concurrent client processes → SIGTERM → socket removed → replay → `diff`. |

### Data: `data/`

| File | Purpose |
|---|---|
| `config.csv` | Window open/closed and the semester credit maximum (270 = 27.0). |
| `buckets.csv` | Bucket credit limits (PC 16.0, PE 6.0, UE 6.0). |
| `courses.csv` | 10 courses with credits (tenths), bucket and prerequisites. |
| `sections.csv` | 12 sections with faculty, VIT-style slots (`A1;TA1;L31;L32`) and capacity. Some are 1-seat sections for contention. |
| `students.csv` | 20 students and their completed courses. |
| `workloads/basic.csv` | One request exercising each rule. |
| `workloads/last_seat.csv` | Contention for the 1-seat sections, plus DROP and re-ADD. |
| `workloads/same_student.csv` | One student filling a timetable up to every limit. |
| `workloads/demo_client_a.csv`, `demo_client_b.csv` | The two competing clients in the demo. |
| `workloads/golden/*.results.csv`, `*.final_state.csv` | Hand-checked expected outputs of the three workloads above. |

### Scripts: `scripts/`

| File | Purpose |
|---|---|
| `gen_workload.py` | A seeded, deterministic workload generator (DROP ratio, share of "hot" single-seat sections, id prefix per client). |
| `bench.py` | The benchmark matrix (sizes × modes × reps). It checks every server run against the oracle and invariants, and writes `metrics.csv`, `summary.md` and `environment.txt`. |
| `plot.py` | The matplotlib throughput and p95-latency bar charts from `metrics.csv`. It refuses to plot incorrect runs. |
| `demo.sh` | The scripted demonstration: a normal request, an invalid request, a malformed message, a two-process last-seat race, shutdown, and the oracle diff. |

### Documentation: `docs/`

| File | Purpose |
|---|---|
| `EXPLAINER.md` | This document. |
| `SCOPE.md` | The specification: rules and order, FCFS definition, input/output formats, benchmark workloads, non-goals. |
| `ARCHITECTURE.md` | The diagram, the ownership/protection table, the correctness and deadlock argument, the trade-off, the modes. |
| `IPC_PROTOCOL.md` | Socket, framing and message grammar, ordering and delivery guarantees. |
| `TESTING.md` | How correctness is established, and the test, TSan and stress commands. |
| `BENCHMARK.md` | Benchmark method, environment, measured results, interpretation. |

### Third party and generated

| Path | Purpose |
|---|---|
| `third_party/doctest/` | The vendored doctest 2.4.11 single-header test framework (MIT licence). |
| `results/` | Generated output: demo runs, benchmark CSV/Markdown/PNG. Never hand-edited. |

## 8. Known limitations

- All state is in memory. Inputs are trusted CSV files with no quoting.
- The rules are simplified FFCS rules (slot-name clash, one section per course, no waitlists).
- FCFS across clients means *admission* order at the server, not the time a client pressed "send".
- Commits are fully serialized, so more workers do not increase throughput (measured).
- Finished connections' reader threads are joined at shutdown rather than immediately. That is fine for a simulation with a handful of clients.

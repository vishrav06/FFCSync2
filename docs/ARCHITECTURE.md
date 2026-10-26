# FFCSync — Architecture

## 1. Processes, threads and ownership

```text
 ┌───────────────────────────────┐     ┌───────────────────────────────┐
 │ ffcsync_client  (process A)   │     │ ffcsync_client  (process B)   │
 │  sender thread ─┐  main thread│     │            ...                │
 │  (window: counting_semaphore) │     │                               │
 └─────────────────┼─────▲───────┘     └──────────────┬──────▲─────────┘
     length-prefixed frames over a Unix domain socket (IPC)   │
 ════════════════════╪═════╪══════════════════════════╪══════╪═════════
 ┌───────────────────▼─────┼──────────────────────────▼──────┼─────────┐
 │ ffcsync_server  (process; SOLE OWNER of registration state)         │
 │                                                                     │
 │  main thread: poll(listen socket, self-pipe) → accept → spawn reader│
 │                                                                     │
 │  reader thread / connection:  read_frame → decode ──MALFORMED──►(reply, seq 0)
 │                         │                                           │
 │                         ▼   ADMISSION POINT  (admission_mu_)        │
 │              seq = next_seq++ ; admission log ; queue.push()        │
 │                         │      (blocks while full = back-pressure)  │
 │                         ▼                                           │
 │           ┌──────────────────────────────┐                          │
 │           │ BoundedQueue<Job>  (FIFO)    │ mutex + not_full/not_empty│
 │           └──────────────┬───────────────┘                          │
 │                          ▼                                          │
 │   worker 1 … worker N:  A) check_static()  — parallel, no lock      │
 │                         B) CommitGate.run_in_order(seq):            │
 │                              wait for turn → check_mutable() →      │
 │                              apply_change() → next turn             │
 │                         C) send response ─────────────────────────► │
 │                                                                     │
 │   Catalog (immutable, lock-free reads)   RegistrationState (gate)   │
 └─────────────────────────────────────────────────────────────────────┘

 ffcsync_seq (separate process, offline): the sequential reference engine.
 It replays the server's admission log so the two outputs can be diffed.
```

| Data | Owner | Protection |
|---|---|---|
| `Catalog` (students, courses, sections, config) | server, loaded at start-up | Immutable. Any thread reads it without a lock. |
| `RegistrationState` (seats, enrolments, credits, slots) and the results log | `ConcurrentEngine` | Only modified inside `CommitGate::run_in_order()` (or under `admission_mu_` when `--workers 0`). |
| `next_seq_`, admission log, `closed_` flag | `ConcurrentEngine` | `admission_mu_` |
| Queue contents | `BoundedQueue` | Its own mutex plus two condition variables |
| A client socket's write side | `Server::Connection` | A per-connection `write_mu` so concurrent worker replies never interleave bytes |

The client never edits server state. It only sends requests and records the answers.

## 2. Life of one request

1. **Client.** The client's sender thread encodes `REQ|R07|ADD|S002|CSE2005-B1` into a 4-byte-length frame and writes it to the socket. A `std::counting_semaphore` limits the number of outstanding requests (`--max-inflight`).
2. **Reader thread.** The server reads exactly one frame (`read_frame` loops over partial reads) and decodes it. A malformed message gets an immediate `MALFORMED` reply with `seq 0` and never touches state.
3. **Admission (FCFS point).** `submit()` locks `admission_mu_`, takes `seq`, logs it and pushes the job onto the bounded queue. If the queue is full, the reader **blocks** there. That back-pressure eventually stops the reader from reading the socket, which in turn throttles the client.
4. **Worker, phase A.** A worker pops the job and evaluates the static rules (window, existence, prerequisites) in parallel with other workers, without any lock.
5. **Worker, phase B.** The worker calls `CommitGate::run_in_order(seq, …)`. It sleeps until `next_ == seq`, then **re-checks every mutable rule against the current state**, commits if allowed, increments `next_` and wakes the waiters.
6. **Worker, phase C.** Outside every lock, the worker writes `RES|R07|7|ACCEPTED|OK|<µs>` to that client's socket under the connection's write mutex.
7. **Client.** The client's main thread matches the response by `request_id` and records latency.

## 3. Why the design is correct

- **One commit order.** Every commit happens inside the gate in seq order. No two commits overlap, and they never reorder. The concurrent server therefore performs exactly the same sequence of state transitions as `RegistrationEngine` processing the admission log. Both call the same function, `decide_and_commit()`.
- **Re-checking at commit time.** A request may wait in the queue while earlier requests take the last seat. Seats, duplicates, clashes and credits are therefore *only* decided inside the gate, never from a stale earlier check. Only immutable facts are checked outside the gate.
- **Why a mutex alone is not enough.** `std::mutex` guarantees mutual exclusion but not *fairness* or *order*. If workers holding seq 7 and seq 8 both block on a mutex, the OS may wake 8 first. Both commits would be safe, since there is no overselling, but the order would violate FCFS: seq 8 could take the last seat that seq 7 should have had. The gate's explicit `next_` counter plus a condition variable enforces order, not just exclusion.
- **No deadlock.**
  - The queue is FIFO and admission pushes in seq order, so workers obtain jobs in seq order. When a worker holds seq *k*, every seq < *k* was popped earlier by a running worker and will reach its turn, so the chain always advances.
  - There is no lock-order cycle. Locks are never nested except `admission_mu_` → queue mutex, always in that order, and workers never take `admission_mu_`.
- **Shutdown is safe.** No thread is detached.
  1. The listening socket is closed and unlinked.
  2. `shutdown(SHUT_RD)` wakes every blocked reader.
  3. The readers are joined.
  4. The queue is closed, and the workers drain everything already admitted and are joined.
  5. Only then are the outputs written.

  Pending responses keep their `Connection` alive through a `shared_ptr` captured in the completion callback. A response to a client that already disconnected fails with `EPIPE` (`MSG_NOSIGNAL`) and is ignored. Its request still commits, because it was admitted.

## 4. The honest trade-off

The commit stage is serialized by design, so only phase A (static checks) and phase C (sending the reply) run in parallel. In this simulation both are tiny compared with socket I/O and thread hand-offs.

The measurements in `BENCHMARK.md` confirm it: one worker is as fast as the inline sequential server, and 2 or 4 workers are **slower**. The extra cost comes from condition-variable hand-offs (`notify_all` wakes every waiting worker, and only one can proceed).

That is the price of a simple, provably FCFS-correct design. A faster design would let non-conflicting requests (different students *and* different sections) commit in parallel, using per-student and per-section ordering. It is explicitly out of scope because it is much harder to explain and to verify.

## 5. Modes

| Mode | Command | Purpose |
|---|---|---|
| Sequential engine | `ffcsync_seq run` | The reference (oracle): in-process, no IPC, no threads |
| Oracle replay | `ffcsync_seq replay` | Re-runs a server admission log for comparison |
| Sequential server | `ffcsync_server --workers 0` | IPC with inline processing under the admission mutex (build-plan phase 3) |
| Concurrent server | `ffcsync_server --workers N` | Queue + worker pool + ordered commit gate (phases 4–5) |
| Single committer | `ffcsync_server --mode committer` | Queue + **one** committer thread that commits batches; no gate (see §6) |

## 6. Single-committer mode

`--mode committer` keeps the reader threads, the admission point and the bounded queue unchanged. It replaces the worker pool and the `CommitGate` with one committer thread, which is the only thread that ever touches `RegistrationState`.

1. `BoundedQueue::pop_batch()` waits until the queue is non-empty, then takes **every** queued job (up to the capacity) in FIFO order.
2. Under `commit_mu_`, the committer runs `check_static()` and `decide_and_commit()` for each job in turn.
3. It releases the lock and sends every reply in the batch.

**Why no gate is needed.** A single consumer of a FIFO queue cannot reorder jobs, and queue order is seq order (§2, step 3). The commits therefore happen in exactly the admission-log order, so the oracle replay argument of §3 applies unchanged. `commit_mu_` exists only so that `state()` and `results()` snapshots taken while the committer runs are consistent; nobody else contends for it.

**Why batch.** Under load, one wake-up of the committer pays for many requests, instead of one condition-variable hand-off and one `notify_all` per commit. With few requests in flight, batches have size 1, and the mode behaves like `--workers 1`. The reader → committer hand-off remains, so it cannot beat the inline `--workers 0` server at low load. See `BENCHMARK.md`.


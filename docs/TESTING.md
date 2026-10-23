# FFCSync — Testing Guide

## Run everything

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure      # unit + concurrency + two-process integration
./build/ffcsync_tests -ts=concurrency          # one doctest suite: rules, invariants, ipc, queue, concurrency
```

**Data-race check** (ThreadSanitizer, a separate build directory):

```bash
cmake -S . -B build-tsan -DFFCSYNC_SANITIZE=thread && cmake --build build-tsan -j
./build-tsan/ffcsync_tests
```

**Stress:** repeat the concurrency suite many times:

```bash
for i in $(seq 20); do ./build/ffcsync_tests -ts=concurrency -m || break; done
```

## How correctness is established

1. **The rules are tested one by one** (`tests/test_rules.cpp`), against the sequential engine and the example dataset. There is a test for each case: successful ADD, window closed, unknown student/section, prerequisite failure, duplicate registration, second section of a course, full section, timetable clash, credit limit (exactly 27.0 accepted, then rejected), bucket limit, invalid DROP, successful DROP (the state returns exactly to the empty state and the seat becomes available again).
2. **Invariant checker** (`check_invariants`, `tests/test_invariants.cpp`). It recomputes seats, credits, bucket credits and timetable slots from the enrolment sets, and checks capacity, uniqueness, limits, clashes and prerequisites. It is tested to accept engine-produced states and to *reject* deliberately corrupted ones. The server runs it on shutdown, and `ffcsync_seq` runs it after every run.
3. **Golden workloads** (`tests/test_golden.cpp`). Three small workloads have stored expected outputs in `data/workloads/golden/`, and any behaviour change shows up as a diff. The expected outcome of each request was hand-checked:

   | Workload | What it covers |
   |---|---|
   | `basic.csv` | One request per rule, in order: R01 OK, R02 PREREQ, R03 UNKNOWN_STUDENT, R04 UNKNOWN_SECTION, R05 ALREADY_REGISTERED, R06 COURSE_ALREADY_TAKEN, R07 OK, R08 SECTION_FULL, R09 OK, R10 TIMETABLE_CLASH, R11/R12 OK, R13 BUCKET_LIMIT, R14 NOT_REGISTERED, R15 DROP OK, R16 OK (the seat released by R15) |
   | `last_seat.csv` | Two single-seat sections. The second requester gets SECTION_FULL, and after a DROP the seat goes to the next ADD |
   | `same_student.csv` | S004 fills a timetable to exactly 270: clash, PC bucket limit, credit limit, drop and re-add, slot freed by a DROP |

4. **The sequential engine is the oracle.** Concurrency tests never hard-code expected outcomes for racing requests. The engine records the admission order it actually chose, and the test replays that order through `RegistrationEngine`. The **per-request results and the final state must be identical**, and the invariants must hold.
5. **Concurrency** (`tests/test_concurrency.cpp`). Every case runs for worker counts {0, 1, 2, 4, 8}.
   - *Last seat*: 8 threads release simultaneously (a `std::latch`) to ADD the 1-seat CSE2005-B1. This is repeated 100×. Exactly one request is accepted, it is the one with seq 1, and the other 7 get SECTION_FULL.
   - *Same student*: ADD/ADD/DROP/DROP/ADD for one section from one thread, with fixed expected results. Meanwhile two other threads submit **clashing** ADDs for the same student; exactly one wins, and it is the lower seq.
   - *Randomized equivalence*: 6 threads × 50 random requests (25 % DROP, including an unknown student), repeated 200× per worker count, each run compared with the oracle.
6. **Queue and worker pool** (`tests/test_queue.cpp`). FIFO order. The capacity is never exceeded under 4 producers and 3 consumers, and nothing is lost or duplicated. A full queue blocks the producer. `close()` releases blocked producers and consumers but still drains items that were already admitted. Engine shutdown processes every admitted request, joins its workers and refuses later submissions.
7. **IPC** (`tests/test_ipc.cpp`):
   - frames split into 1-byte writes
   - oversized and truncated frames
   - codec round-trips and malformed payloads
   - the server's answers equal the oracle's (with 0 and 2 workers)
   - MALFORMED handling, and continued service after a bad client
   - a disconnecting client's admitted request still commits
   - the socket file is created, refused when a live server holds it, replaced when stale, and removed on shutdown
8. **Two-process integration** (`tests/integration.sh`, run by CTest). This uses the real `ffcsync_server` with 4 workers and a queue of 8, and **three concurrent client processes** sending 600 requests each. It stops the server with SIGTERM, checks that the socket file is gone and that the invariants are OK, then runs `ffcsync_seq replay` on the admission log and `diff`s `results.csv` and `final_state.csv`.

## Checking a manual run against the reference

```bash
./build/ffcsync_server --workers 4 --out results/run1 &
./build/ffcsync_client --requests data/workloads/basic.csv
kill -TERM %1; wait
./build/ffcsync_seq replay --admission-log results/run1/admission.csv --out results/run1/oracle
diff results/run1/results.csv results/run1/oracle/results.csv && \
diff results/run1/final_state.csv results/run1/oracle/final_state.csv && echo MATCH
```

With a single client sending a file, admission order equals file order, so the server output also equals `ffcsync_seq run` on that file. In particular it equals `data/workloads/golden/*.results.csv`.

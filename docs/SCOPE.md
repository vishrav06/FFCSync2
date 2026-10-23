# FFCSync — Scope and Specification

FFCSync is a teaching simulation of VIT's FFCS course registration during a high-demand registration window. It shows how concurrent requests compete for shared resources, and how processes, IPC, threads, synchronization and request ordering keep the outcome correct.

**Primary correctness rule:** a request must never take a seat that is no longer available, never violate a registration rule, and never produce a result that differs from the FCFS policy defined below.

## 1. Request types

| Op     | Meaning |
|--------|---------|
| `ADD`  | Register student *S* in section *X*. |
| `DROP` | Remove student *S* from section *X*. |

## 2. Registration rules and rejection reasons

Rules are evaluated **in this order**, and the **first** rule that fails is reported. Credits are integers in tenths (4.0 credits = `40`), so every comparison is exact.

| # | Applies to | Rule | Reason code |
|---|---|---|---|
| 0 | IPC only | Message is well-formed (`REQ\|id\|ADD/DROP\|student\|section`) | `MALFORMED` |
| 1 | all  | Registration window is open (`config.csv: window_open`) | `WINDOW_CLOSED` |
| 2 | all  | Student exists | `UNKNOWN_STUDENT` |
| 3 | all  | Section exists | `UNKNOWN_SECTION` |
| 4 | ADD  | All prerequisites of the course are in the student's completed courses | `PREREQ_NOT_MET` |
| 5 | ADD  | Student is not already in this section | `ALREADY_REGISTERED` |
| 6 | ADD  | Student is not in another section of the same course | `COURSE_ALREADY_TAKEN` |
| 7 | ADD  | Section has a free seat (`enrolled < capacity`) | `SECTION_FULL` |
| 8 | ADD  | None of the section's slots is already in the student's timetable | `TIMETABLE_CLASH` |
| 9 | ADD  | `credits + course credits <= max_credits_tenths` (270 = 27.0) | `CREDIT_LIMIT` |
| 10 | ADD | `bucket credits + course credits <= bucket limit` (if the bucket is configured) | `BUCKET_LIMIT` |
| 11 | DROP | Student is registered in this section | `NOT_REGISTERED` |

An accepted ADD takes one seat and adds the course's credits, bucket credits and timetable slots to the student. An accepted DROP reverses all of these exactly.

Rules 1–4 are **static**: they only read data that never changes after start-up. Rules 5–11 are **mutable**: they read registration state that other requests change. That split is the basis of the concurrency design (see `ARCHITECTURE.md`).

Simplifications (documented on purpose):
- Two sections clash if they share any slot name. Theory and lab slots are treated alike.
- There are no pre-existing registrations. Every run starts from an empty registration state.
- Re-registering a course that is already in `completed_courses` is not checked.

## 3. FCFS definition

- **Admission point.** A request is *admitted* when the server's connection-reader thread calls `ConcurrentEngine::submit()`. Inside that call, under one admission mutex, the request receives the next sequence number (`seq = 1, 2, 3, …`) and is appended to the admission log and the FIFO queue. The mutex is released only after both steps. A malformed message is rejected before admission and gets `seq = 0`.
- **Guarantee.** Requests commit (re-check the mutable rules and change the state) **in strictly increasing seq order**. Therefore the result of every request, and the final state, are exactly what the sequential reference engine produces when it processes the admission log in seq order.
- **What FCFS does *not* promise.**
  - Between different clients, "first" means *first to be admitted by the server*, not first to call `send()`. Two clients racing for the last seat are ordered by whichever request the server admits first, and that decision is recorded in the admission log.
  - Requests from **one** connection are admitted in the order the client sent them.
  - Responses may be **delivered** out of seq order when several workers are running. Clients match responses by `request_id`.

## 4. Inputs

All inputs are CSV files. The first line is a header. Fields are separated by `,` and trimmed, and quoting is not supported. List fields use `;`. Blank lines and `#` comment lines are ignored.

| File | Columns |
|---|---|
| `data/config.csv`   | `key,value` with keys `window_open` (0/1) and `max_credits_tenths` |
| `data/buckets.csv`  | `bucket,max_tenths` |
| `data/courses.csv`  | `course_code,title,credits_tenths,bucket,prereqs` (prereqs `;`-separated) |
| `data/sections.csv` | `section_id,course_code,faculty,slots,capacity` (slots `;`-separated) |
| `data/students.csv` | `student_id,name,completed_courses` (`;`-separated) |
| request file        | `request_id,op,student_id,section_id` |

The example dataset has 20 students, 10 courses and 12 sections. Buckets are PC (limit 16.0), PE (6.0) and UE (6.0), and the semester maximum is 27.0 credits. `CSE2005-B1` and `CSE4001-E1` have a single seat each, for contention demos.

## 5. Outputs

| File | Columns / content |
|---|---|
| `results.csv`     | `seq,request_id,op,student_id,section_id,status,reason`, one row per request in seq order |
| `admission.csv`   | `seq,request_id,op,student_id,section_id` (server only: the FCFS order) |
| `final_state.csv` | `kind,id,a,b,c`: `section,<id>,<enrolled>,<capacity>,` and `student,<id>,<credits>,<sections>,<bucket=credits;…>` |
| `summary.txt`     | counts per status and reason, queue statistics, invariant verdict |
| client `--out`    | `request_id,op,student_id,section_id,seq,status,reason,latency_us,server_us` |
| `metrics.csv`     | benchmark rows (see `BENCHMARK.md`) |

All outputs are sorted (by seq or by id), so two runs can be compared with `diff`.

## 6. Benchmark workloads

`scripts/gen_workload.py` generates them deterministically from a seed. By default 25 % of requests are DROPs, and half of the ADDs target the single-seat "hot" sections. The benchmark matrix is {1 000, 10 000} requests × {sequential engine, server with 0/1/2/4 workers}, using 4 client processes and 3 repetitions.

## 7. Non-goals

- No web or mobile frontend, database, cloud deployment, authentication, payment, notifications, or real VIT integration.
- No custom lock manager, scheduler or semaphore. Only standard `std::mutex`, `std::condition_variable`, `std::counting_semaphore` and `std::latch` are used.
- No CPU pinning, cache-line padding, spin-then-sleep, lock sharding, admission control or other micro-optimizations.
- No deadlock detection/recovery subsystem. The design avoids deadlock by construction (see `ARCHITECTURE.md`).
- No large-scale benchmark automation or ablation studies.
- No persistence: all state is in memory and is lost when the server exits. Output files are a report, not a database.

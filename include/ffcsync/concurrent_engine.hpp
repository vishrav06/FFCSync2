// The server's request-processing pipeline (independent of sockets, so it can
// be tested directly):
//
//   submit()  --[admission: assign seq]-->  BoundedQueue  -->  worker threads
//                                                                 |
//                       check_static() in parallel, no lock  <----+
//                                                                 |
//                       CommitGate: in seq order, re-check mutable rules + commit
//                                                                 |
//                       completion callback (e.g. send the IPC response)
//
// FCFS admission point: inside submit(), under admission_mu_, the request
// receives seq = next_seq_++ and is pushed into the queue before the mutex is
// released. Therefore queue order == seq order == admission-log order.
//
// workers == 0 selects the sequential server mode (build-plan phase 3): the
// request is processed inline in the calling thread while admission_mu_ is
// held. No queue, no worker threads.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "ffcsync/commit_gate.hpp"
#include "ffcsync/model.hpp"
#include "ffcsync/request_queue.hpp"

namespace ffcsync {

struct EngineOptions {
    int workers = 4;                  // 0 = sequential inline processing
    std::size_t queue_capacity = 64;
    bool verbose = false;             // per-request thread/queue log on stderr
};

// Called exactly once per admitted request, from the thread that committed it.
using Completion = std::function<void(const RegistrationResult&, std::uint64_t server_us)>;

class ConcurrentEngine {
public:
    ConcurrentEngine(const Catalog& catalog, EngineOptions options);
    ~ConcurrentEngine();

    ConcurrentEngine(const ConcurrentEngine&) = delete;
    ConcurrentEngine& operator=(const ConcurrentEngine&) = delete;

    // The FCFS admission point. Assigns the next sequence number and enqueues
    // the request, blocking while the queue is full. Returns the seq, or 0 if
    // the engine has been shut down (the request is then not processed).
    std::uint64_t submit(const RegistrationRequest& req, Completion on_done);

    // Stops admission, lets the workers drain the queue, joins them. Idempotent.
    void shutdown();

    // Snapshots (thread-safe; most meaningful after shutdown()).
    RegistrationState state() const;
    std::vector<RegistrationResult> results() const;      // in commit (= seq) order
    std::vector<RegistrationResult> admissions() const;   // in admission (= seq) order
    std::size_t queue_high_water() const { return queue_.high_water(); }
    const EngineOptions& options() const { return options_; }

private:
    struct Job {
        std::uint64_t seq;
        RegistrationRequest request;
        Completion on_done;
    };

    void worker_loop(int worker_id);
    RegistrationResult commit(std::uint64_t seq, const RegistrationRequest& req, Reason static_verdict);
    void log(const std::string& line);

    const Catalog& catalog_;
    const EngineOptions options_;

    mutable std::mutex admission_mu_;   // guards next_seq_, admissions_, closed_
    std::uint64_t next_seq_ = 1;
    std::vector<RegistrationResult> admissions_;
    bool closed_ = false;

    BoundedQueue<Job> queue_;
    CommitGate gate_;

    // Mutable registration state. Guarded by gate_ (worker mode) or by
    // admission_mu_ (inline mode). Never touched outside those.
    RegistrationState state_;
    std::vector<RegistrationResult> results_;

    std::vector<std::thread> workers_;
    std::mutex log_mu_;
};

}  // namespace ffcsync

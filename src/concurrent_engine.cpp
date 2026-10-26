#include "ffcsync/concurrent_engine.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

#include "ffcsync/rules.hpp"

namespace ffcsync {

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t micros_since(Clock::time_point t0) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
}

}  // namespace

const char* to_string(CommitMode mode) {
    return mode == CommitMode::Committer ? "committer" : "gate";
}

ConcurrentEngine::ConcurrentEngine(const Catalog& catalog, EngineOptions options)
    : catalog_(catalog),
      options_(options),
      queue_(options.queue_capacity),
      state_(RegistrationState::empty_for(catalog)) {
    if (options_.workers < 0) throw std::invalid_argument("workers must be >= 0");
    if (committer_mode()) {
        workers_.emplace_back(&ConcurrentEngine::committer_loop, this);
        return;
    }
    for (int i = 0; i < options_.workers; ++i) {
        workers_.emplace_back(&ConcurrentEngine::worker_loop, this, i + 1);
    }
}

ConcurrentEngine::~ConcurrentEngine() { shutdown(); }

std::uint64_t ConcurrentEngine::submit(const RegistrationRequest& req, Completion on_done) {
    std::unique_lock lock(admission_mu_);
    if (closed_) return 0;
    const std::uint64_t seq = next_seq_++;
    admissions_.push_back(RegistrationResult{seq, req, Status::Rejected, Reason::Ok});

    if (options_.workers == 0) {
        // Sequential server mode: process right here, still holding admission_mu_.
        const auto t0 = Clock::now();
        RegistrationResult result = commit(seq, req, check_static(catalog_, req));
        const auto us = micros_since(t0);
        lock.unlock();
        if (on_done) on_done(result, us);
        return seq;
    }

    // Pushing while holding admission_mu_ keeps queue order identical to seq
    // order. If the queue is full this blocks the producer (back-pressure);
    // workers only need the queue's own mutex, so they keep draining.
    if (options_.verbose) {
        log("[admit ] seq=" + std::to_string(seq) + " id=" + req.request_id +
            " queue=" + std::to_string(queue_.size()) + "/" + std::to_string(queue_.capacity()));
    }
    queue_.push(Job{seq, req, std::move(on_done)});
    return seq;
}

void ConcurrentEngine::worker_loop(int worker_id) {
    while (auto job = queue_.pop()) {
        const auto t0 = Clock::now();

        // Phase A (parallel, no lock): rules that only read the immutable catalog.
        const Reason static_verdict = check_static(catalog_, job->request);

        // Phase B (serialised, in seq order): re-check mutable facts and commit.
        RegistrationResult result = gate_.run_in_order(
            job->seq, [&] { return commit(job->seq, job->request, static_verdict); });
        const auto us = micros_since(t0);

        if (options_.verbose) {
            log("[worker " + std::to_string(worker_id) + "] seq=" + std::to_string(job->seq) +
                " " + std::string(to_string(result.status)) + " " +
                std::string(to_string(result.reason)));
        }
        // Phase C (parallel): deliver the result, outside every lock.
        if (job->on_done) job->on_done(result, us);
    }
}

void ConcurrentEngine::committer_loop() {
    struct Done {
        RegistrationResult result;
        std::uint64_t us;
    };
    std::vector<Job> batch;
    std::vector<Done> done;
    while (queue_.pop_batch(batch, queue_.capacity())) {
        done.clear();
        {
            // This thread is the only one that commits, and it takes jobs in
            // queue (= seq) order, so commits are in seq order without a gate.
            std::lock_guard lock(commit_mu_);
            for (const auto& job : batch) {
                const auto t0 = Clock::now();
                RegistrationResult result =
                    commit(job.seq, job.request, check_static(catalog_, job.request));
                done.push_back(Done{std::move(result), micros_since(t0)});
            }
        }
        if (options_.verbose) {
            log("[commit ] seq=" + std::to_string(batch.front().seq) + ".." +
                std::to_string(batch.back().seq) + " batch=" + std::to_string(batch.size()));
        }
        // Deliver the results outside the lock.
        for (std::size_t i = 0; i < batch.size(); ++i) {
            if (batch[i].on_done) batch[i].on_done(done[i].result, done[i].us);
        }
    }
}

RegistrationResult ConcurrentEngine::commit(std::uint64_t seq, const RegistrationRequest& req,
                                            Reason static_verdict) {
    RegistrationResult result = decide_and_commit(catalog_, state_, seq, req, static_verdict);
    results_.push_back(result);
    return result;
}

void ConcurrentEngine::shutdown() {
    {
        std::lock_guard lock(admission_mu_);
        if (closed_) return;
        closed_ = true;
    }
    queue_.close();   // workers finish every queued job, then pop() returns nullopt
    for (auto& t : workers_) t.join();
    workers_.clear();
}

RegistrationState ConcurrentEngine::state() const {
    if (options_.workers == 0) {
        std::lock_guard lock(admission_mu_);
        return state_;
    }
    if (committer_mode()) {
        std::lock_guard lock(commit_mu_);
        return state_;
    }
    return gate_.inspect([&] { return state_; });
}

std::vector<RegistrationResult> ConcurrentEngine::results() const {
    if (options_.workers == 0) {
        std::lock_guard lock(admission_mu_);
        return results_;
    }
    if (committer_mode()) {
        std::lock_guard lock(commit_mu_);
        return results_;
    }
    return gate_.inspect([&] { return results_; });
}

std::vector<RegistrationResult> ConcurrentEngine::admissions() const {
    std::lock_guard lock(admission_mu_);
    return admissions_;
}

void ConcurrentEngine::log(const std::string& line) {
    std::lock_guard lock(log_mu_);
    std::cerr << line << '\n';
}

}  // namespace ffcsync

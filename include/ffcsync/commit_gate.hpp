// Ordered commit stage ("ticket turnstile").
//
// Every admitted request carries a sequence number 1, 2, 3, ... A worker
// holding request `seq` calls run_in_order(seq, f): it sleeps until every
// request with a smaller seq has committed, then runs `f` while holding the
// gate's mutex, then advances the turn and wakes the other waiters.
//
// Why not just a mutex? std::mutex makes no promise about WHICH waiting
// thread acquires it next, so two workers holding seq 7 and seq 8 could
// commit in the order 8, 7. The explicit `next_` counter is what turns
// "mutual exclusion" into "mutual exclusion in FCFS order".
//
// Liveness: workers take jobs from a FIFO queue in seq order, so by the time
// a worker holds seq k, every seq < k is already held by some live worker
// (or has committed). Each of those will eventually get its turn, so the
// waiting chain always makes progress and cannot deadlock.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace ffcsync {

class CommitGate {
public:
    template <typename F>
    auto run_in_order(std::uint64_t seq, F&& f) {
        std::unique_lock lock(mu_);
        turn_.wait(lock, [&] { return next_ == seq; });
        // On scope exit (normal return or exception): advance the turn,
        // release the mutex and wake the waiters so the next seq can go.
        struct PassTurn {
            CommitGate& gate;
            std::unique_lock<std::mutex>& lock;
            ~PassTurn() {
                ++gate.next_;
                lock.unlock();
                gate.turn_.notify_all();
            }
        } pass{*this, lock};
        return f();
    }

    // Runs `f` under the gate's mutex without taking a turn (read-only snapshots).
    template <typename F>
    auto inspect(F&& f) const {
        std::lock_guard lock(mu_);
        return f();
    }

private:
    mutable std::mutex mu_;
    std::condition_variable turn_;
    std::uint64_t next_ = 1;
};

}  // namespace ffcsync

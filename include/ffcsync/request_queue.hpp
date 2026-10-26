// Bounded, blocking, thread-safe FIFO queue (classic producer/consumer).
//
//   push() blocks while the queue is full   -> back-pressure on producers
//   pop()  blocks while the queue is empty  -> idle workers sleep
//   pop_batch() like pop(), but takes every queued item (up to a maximum)
//   close() wakes everyone: further pushes fail, pops drain what is left
//          and then return std::nullopt so workers can exit.
//
// One mutex protects the deque; two condition variables signal "space
// available" and "item available". The queue itself is FIFO: items leave in
// exactly the order they were pushed.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace ffcsync {

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) throw std::invalid_argument("queue capacity must be > 0");
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // Blocks while full. Returns false (and drops the item) if the queue is closed.
    bool push(T item) {
        std::unique_lock lock(mu_);
        not_full_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
        if (closed_) return false;
        items_.push_back(std::move(item));
        if (items_.size() > high_water_) high_water_ = items_.size();
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    // Blocks while empty. Returns nullopt once the queue is closed and drained.
    std::optional<T> pop() {
        std::unique_lock lock(mu_);
        not_empty_.wait(lock, [&] { return closed_ || !items_.empty(); });
        if (items_.empty()) return std::nullopt;
        T item = std::move(items_.front());
        items_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return item;
    }

    // Blocks while empty, then moves up to `max` items (FIFO order) into `out`.
    // Returns false once the queue is closed and drained (`out` is then empty).
    bool pop_batch(std::vector<T>& out, std::size_t max) {
        out.clear();
        std::unique_lock lock(mu_);
        not_empty_.wait(lock, [&] { return closed_ || !items_.empty(); });
        if (items_.empty()) return false;
        while (!items_.empty() && out.size() < max) {
            out.push_back(std::move(items_.front()));
            items_.pop_front();
        }
        lock.unlock();
        not_full_.notify_all();   // several slots may have been freed
        return true;
    }

    void close() {
        {
            std::lock_guard lock(mu_);
            closed_ = true;
        }
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard lock(mu_);
        return items_.size();
    }

    // Largest size ever observed (used by tests and the demo log).
    std::size_t high_water() const {
        std::lock_guard lock(mu_);
        return high_water_;
    }

    std::size_t capacity() const { return capacity_; }

private:
    const std::size_t capacity_;
    mutable std::mutex mu_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::deque<T> items_;
    std::size_t high_water_ = 0;
    bool closed_ = false;
};

}  // namespace ffcsync

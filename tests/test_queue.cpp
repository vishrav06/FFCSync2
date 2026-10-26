// Bounded queue and worker-pool lifecycle tests.
#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

#include "doctest.h"
#include "fixtures.hpp"
#include "ffcsync/concurrent_engine.hpp"
#include "ffcsync/request_queue.hpp"

using namespace ffcsync;
using namespace ffcsync::test;
using namespace std::chrono_literals;

TEST_SUITE("queue") {

TEST_CASE("FIFO order") {
    BoundedQueue<int> q(4);
    for (int i = 0; i < 4; ++i) CHECK(q.push(i));
    for (int i = 0; i < 4; ++i) CHECK(*q.pop() == i);
}

TEST_CASE("capacity is never exceeded and nothing is lost under contention") {
    constexpr int kProducers = 4, kConsumers = 3, kPerProducer = 2000;
    BoundedQueue<int> q(8);
    std::mutex seen_mu;
    std::multiset<int> seen;
    std::vector<std::thread> threads;
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&] {
            while (auto v = q.pop()) {
                CHECK(q.size() <= q.capacity());
                std::lock_guard lock(seen_mu);
                seen.insert(*v);
            }
        });
    }
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) q.push(p * kPerProducer + i);
        });
    }
    for (auto& t : producers) t.join();
    q.close();
    for (auto& t : threads) t.join();

    CHECK(q.high_water() <= 8);
    REQUIRE(seen.size() == kProducers * kPerProducer);
    CHECK(std::set<int>(seen.begin(), seen.end()).size() == seen.size());   // no duplicates
}

TEST_CASE("push blocks the producer while the queue is full") {
    BoundedQueue<int> q(2);
    q.push(1);
    q.push(2);
    std::atomic<bool> pushed{false};
    std::thread producer([&] {
        q.push(3);
        pushed = true;
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(pushed.load());   // still blocked: queue full
    CHECK(*q.pop() == 1);
    producer.join();
    CHECK(pushed.load());
    CHECK(q.size() == 2);
}

TEST_CASE("close drains remaining items, then wakes and releases everyone") {
    BoundedQueue<int> q(1);
    q.push(7);
    std::atomic<int> blocked_push_result{-1};
    std::thread producer([&] { blocked_push_result = q.push(8) ? 1 : 0; });   // blocks: full
    std::this_thread::sleep_for(20ms);
    q.close();
    producer.join();
    CHECK(blocked_push_result == 0);
    CHECK(*q.pop() == 7);      // item admitted before close is still delivered
    CHECK_FALSE(q.pop());      // closed and empty
    CHECK_FALSE(q.push(9));

    BoundedQueue<int> empty(1);
    std::thread consumer([&] { CHECK_FALSE(empty.pop()); });   // blocks: empty
    std::this_thread::sleep_for(20ms);
    empty.close();
    consumer.join();
}

TEST_CASE("pop_batch takes up to max items in FIFO order") {
    BoundedQueue<int> q(8);
    for (int i = 0; i < 5; ++i) q.push(i);
    std::vector<int> batch{99};   // stale contents are cleared
    REQUIRE(q.pop_batch(batch, 3));
    CHECK(batch == std::vector<int>{0, 1, 2});
    REQUIRE(q.pop_batch(batch, 8));
    CHECK(batch == std::vector<int>{3, 4});   // only what is queued, no waiting for more
    CHECK(q.size() == 0);
}

TEST_CASE("pop_batch releases blocked producers and drains after close") {
    BoundedQueue<int> q(2);
    q.push(1);
    q.push(2);
    std::atomic<int> pushed{0};
    std::vector<std::thread> producers;
    for (int i = 3; i <= 4; ++i) {
        producers.emplace_back([&, i] {
            q.push(i);
            ++pushed;
        });
    }
    std::this_thread::sleep_for(50ms);
    CHECK(pushed == 0);   // both blocked: queue full
    std::vector<int> batch;
    REQUIRE(q.pop_batch(batch, 2));   // frees two slots at once: both producers must wake
    CHECK(batch == std::vector<int>{1, 2});
    for (auto& t : producers) t.join();
    CHECK(pushed == 2);

    q.close();
    REQUIRE(q.pop_batch(batch, 8));   // admitted before close: still delivered
    CHECK(batch.size() == 2);
    CHECK_FALSE(q.pop_batch(batch, 8));
    CHECK(batch.empty());

    BoundedQueue<int> empty(1);
    std::thread consumer([&] {
        std::vector<int> b;
        CHECK_FALSE(empty.pop_batch(b, 4));   // blocks: empty, then closed
    });
    std::this_thread::sleep_for(20ms);
    empty.close();
    consumer.join();
}

TEST_CASE("engine shutdown processes every admitted request and joins workers") {
    const std::pair<int, CommitMode> configs[] = {{1, CommitMode::Gate},
                                                  {2, CommitMode::Gate},
                                                  {4, CommitMode::Gate},
                                                  {8, CommitMode::Gate},
                                                  {1, CommitMode::Committer}};
    for (const auto& [workers, mode] : configs) {
        CAPTURE(workers);
        CAPTURE(to_string(mode));
        std::atomic<int> completed{0};
        ConcurrentEngine engine(catalog(), EngineOptions{workers, 4, false, mode});
        std::vector<std::thread> submitters;
        for (int t = 0; t < 4; ++t) {
            submitters.emplace_back([&, t] {
                for (int i = 0; i < 250; ++i) {
                    auto seq = engine.submit(add("S00" + std::to_string(1 + (i % 9)), "HUM1021-A2",
                                                 std::to_string(t) + "-" + std::to_string(i)),
                                             [&](const RegistrationResult&, std::uint64_t) {
                                                 ++completed;
                                             });
                    CHECK(seq > 0);
                }
            });
        }
        for (auto& t : submitters) t.join();
        engine.shutdown();
        CHECK(completed == 1000);
        CHECK(engine.results().size() == 1000);
        CHECK(engine.queue_high_water() <= 4);
        CHECK(engine.submit(add("S001", "HUM1021-A2"), nullptr) == 0);   // closed
        engine.shutdown();                                               // idempotent
    }
}

}  // TEST_SUITE

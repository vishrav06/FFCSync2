// FCFS correctness under concurrency.
//
// Strategy: many threads submit concurrently, so the admission order is
// nondeterministic. The engine records that order (seq numbers). Replaying
// it through the sequential RegistrationEngine must reproduce exactly the
// same per-request results and the same final state, for every engine
// configuration (inline, gate with N workers, single committer).
#include <latch>
#include <map>
#include <random>
#include <thread>
#include <vector>

#include "doctest.h"
#include "fixtures.hpp"
#include "ffcsync/concurrent_engine.hpp"
#include "ffcsync/invariants.hpp"
#include "ffcsync/registration_engine.hpp"

using namespace ffcsync;
using namespace ffcsync::test;

namespace {

struct EngineConfig {
    int workers;
    CommitMode mode;
};
constexpr EngineConfig kConfigs[] = {{0, CommitMode::Gate},
                               {1, CommitMode::Gate},
                               {2, CommitMode::Gate},
                               {4, CommitMode::Gate},
                               {8, CommitMode::Gate},
                               {1, CommitMode::Committer}};

EngineOptions options_for(const EngineConfig& c, std::size_t queue_capacity) {
    return EngineOptions{c.workers, queue_capacity, false, c.mode};
}

// Submits each list of requests from its own thread, all starting together.
// Returns the results delivered to the completion callbacks, keyed by seq.
std::map<std::uint64_t, RegistrationResult> submit_concurrently(
    ConcurrentEngine& engine, const std::vector<std::vector<RegistrationRequest>>& per_thread) {
    std::mutex mu;
    std::map<std::uint64_t, RegistrationResult> delivered;
    std::latch start(static_cast<std::ptrdiff_t>(per_thread.size()));
    std::vector<std::thread> threads;
    for (const auto& reqs : per_thread) {
        threads.emplace_back([&, &reqs = reqs] {
            start.arrive_and_wait();
            for (const auto& r : reqs) {
                engine.submit(r, [&](const RegistrationResult& res, std::uint64_t) {
                    std::lock_guard lock(mu);
                    CHECK(delivered.emplace(res.seq, res).second);   // exactly once
                });
            }
        });
    }
    for (auto& t : threads) t.join();
    engine.shutdown();
    return delivered;
}

// The core oracle check.
void check_matches_oracle(const ConcurrentEngine& engine,
                          const std::map<std::uint64_t, RegistrationResult>& delivered) {
    const auto admitted = engine.admissions();
    const auto results = engine.results();
    REQUIRE(results.size() == admitted.size());
    REQUIRE(delivered.size() == admitted.size());

    RegistrationEngine oracle(catalog());
    for (std::size_t i = 0; i < admitted.size(); ++i) {
        REQUIRE(admitted[i].seq == i + 1);   // admission log is gap-free and ordered
        const auto expected = oracle.apply_with_seq(admitted[i].seq, admitted[i].request);
        const auto& got = results[i];
        CHECK(got.seq == expected.seq);   // commits happened in seq order
        CHECK(got.request.request_id == expected.request.request_id);
        CHECK(got.status == expected.status);
        CHECK(got.reason == expected.reason);
        CHECK(delivered.at(expected.seq).reason == expected.reason);
    }
    CHECK(engine.state() == oracle.state());
    auto violations = check_invariants(catalog(), engine.state());
    CHECK(violations.empty());
}

}  // namespace

TEST_SUITE("concurrency") {

TEST_CASE("students racing for the last seat: exactly one wins, the earliest seq") {
    // All of these students satisfy the CSE2005 prerequisite. CSE2005-B1 has 1 seat.
    const std::vector<std::string> students = {"S001", "S002", "S004", "S006",
                                               "S007", "S009", "S010", "S011"};
    for (const EngineConfig& cfg : kConfigs) {
        CAPTURE(cfg.workers);
        CAPTURE(to_string(cfg.mode));
        for (int rep = 0; rep < 100; ++rep) {
            ConcurrentEngine engine(catalog(), options_for(cfg, 4));
            std::vector<std::vector<RegistrationRequest>> per_thread;
            for (const auto& s : students) per_thread.push_back({add(s, "CSE2005-B1", s)});
            auto delivered = submit_concurrently(engine, per_thread);

            int accepted = 0;
            for (const auto& [seq, r] : delivered) {
                if (r.status == Status::Accepted) {
                    ++accepted;
                    CHECK(seq == 1);   // FCFS: the first admitted request wins
                } else {
                    CHECK(r.reason == Reason::SectionFull);
                }
            }
            CHECK(accepted == 1);
            CHECK(engine.state().enrolled.at("CSE2005-B1") == 1);
            check_matches_oracle(engine, delivered);
        }
    }
}

TEST_CASE("conflicting requests from the same student") {
    for (const EngineConfig& cfg : kConfigs) {
        CAPTURE(cfg.workers);
        CAPTURE(to_string(cfg.mode));
        for (int rep = 0; rep < 50; ++rep) {
            ConcurrentEngine engine(catalog(), options_for(cfg, 4));
            // Thread 0: an ordered sequence for S004 whose outcomes depend on each other.
            // Threads 1-2: S004 again, two clashing ADDs racing each other (slot B1),
            //              while other students register in unrelated sections.
            std::vector<std::vector<RegistrationRequest>> per_thread = {
                {add("S004", "CSE3001-D1", "a1"), add("S004", "CSE3001-D1", "a2"),
                 drop("S004", "CSE3001-D1", "a3"), drop("S004", "CSE3001-D1", "a4"),
                 add("S004", "CSE3001-D1", "a5")},
                {add("S004", "CSE2005-B1", "b1"), add("S010", "MAT2001-B1", "b2")},
                {add("S004", "MAT2001-B1", "c1"), add("S007", "CSE2005-A1", "c2")},
            };
            auto delivered = submit_concurrently(engine, per_thread);
            check_matches_oracle(engine, delivered);

            std::map<std::string, RegistrationResult> by_id;
            for (const auto& [seq, r] : delivered) by_id[r.request.request_id] = r;
            // Same-thread requests keep their relative order, so these are fixed:
            CHECK(by_id["a1"].reason == Reason::Ok);
            CHECK(by_id["a2"].reason == Reason::AlreadyRegistered);
            CHECK(by_id["a3"].reason == Reason::Ok);
            CHECK(by_id["a4"].reason == Reason::NotRegistered);
            CHECK(by_id["a5"].reason == Reason::Ok);
            // b1 and c1 clash on slot B1: exactly one wins, and it is the earlier seq.
            const auto& b1 = by_id["b1"];
            const auto& c1 = by_id["c1"];
            CHECK((b1.status == Status::Accepted) != (c1.status == Status::Accepted));
            const auto& winner = b1.seq < c1.seq ? b1 : c1;
            const auto& loser = b1.seq < c1.seq ? c1 : b1;
            CHECK(winner.status == Status::Accepted);
            CHECK(loser.reason == Reason::TimetableClash);
        }
    }
}

TEST_CASE("randomised workloads match the sequential reference") {
    std::vector<StudentId> students;
    for (const auto& [id, s] : catalog().students) students.push_back(id);
    students.push_back("S999");   // unknown student
    std::vector<SectionId> sections;
    for (const auto& [id, s] : catalog().sections) sections.push_back(id);

    for (const EngineConfig& cfg : kConfigs) {
        CAPTURE(cfg.workers);
        CAPTURE(to_string(cfg.mode));
        for (unsigned rep = 0; rep < 200; ++rep) {
            CAPTURE(rep);
            std::mt19937 rng(rep * 7919u + static_cast<unsigned>(cfg.workers) +
                             (cfg.mode == CommitMode::Committer ? 100u : 0u));
            constexpr int kThreads = 6, kPerThread = 50;
            std::vector<std::vector<RegistrationRequest>> per_thread(kThreads);
            for (int t = 0; t < kThreads; ++t) {
                for (int i = 0; i < kPerThread; ++i) {
                    RegistrationRequest r;
                    r.request_id = std::to_string(t) + "-" + std::to_string(i);
                    r.op = rng() % 4 == 0 ? Op::Drop : Op::Add;
                    r.student_id = students[rng() % students.size()];
                    r.section_id = sections[rng() % sections.size()];
                    per_thread[t].push_back(r);
                }
            }
            ConcurrentEngine engine(catalog(), options_for(cfg, 8));
            auto delivered = submit_concurrently(engine, per_thread);
            check_matches_oracle(engine, delivered);
        }
    }
}

TEST_CASE("batched submissions get consecutive seqs and match the sequential reference") {
    std::vector<StudentId> students;
    for (const auto& [id, s] : catalog().students) students.push_back(id);
    std::vector<SectionId> sections;
    for (const auto& [id, s] : catalog().sections) sections.push_back(id);

    for (const EngineConfig& cfg : kConfigs) {
        CAPTURE(cfg.workers);
        CAPTURE(to_string(cfg.mode));
        for (unsigned rep = 0; rep < 50; ++rep) {
            CAPTURE(rep);
            std::mt19937 rng(rep * 104729u + static_cast<unsigned>(cfg.workers));
            ConcurrentEngine engine(catalog(), options_for(cfg, 8));
            std::mutex mu;
            std::map<std::uint64_t, RegistrationResult> delivered;
            std::vector<std::vector<std::vector<RegistrationRequest>>> batches(4);
            for (unsigned t = 0; t < batches.size(); ++t) {
                for (int b = 0; b < 10; ++b) {
                    std::vector<RegistrationRequest> batch(1 + rng() % 20);   // may exceed capacity
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                        batch[i] = RegistrationRequest{
                            std::to_string(t) + "-" + std::to_string(b) + "-" + std::to_string(i),
                            rng() % 4 == 0 ? Op::Drop : Op::Add, students[rng() % students.size()],
                            sections[rng() % sections.size()]};
                    }
                    batches[t].push_back(std::move(batch));
                }
            }
            std::latch start(static_cast<std::ptrdiff_t>(batches.size()));
            std::vector<std::thread> threads;
            for (const auto& mine : batches) {
                threads.emplace_back([&, &mine = mine] {
                    start.arrive_and_wait();
                    for (const auto& batch : mine) {
                        const auto first = engine.submit_batch(
                            batch, [&](const RegistrationResult& res, std::uint64_t) {
                                std::lock_guard lock(mu);
                                CHECK(delivered.emplace(res.seq, res).second);
                            });
                        REQUIRE(first > 0);
                        // The batch occupies first .. first+size-1 in span order.
                        const auto admitted = engine.admissions();
                        for (std::size_t i = 0; i < batch.size(); ++i) {
                            CHECK(admitted[first - 1 + i].request.request_id == batch[i].request_id);
                        }
                    }
                });
            }
            for (auto& t : threads) t.join();
            engine.shutdown();
            check_matches_oracle(engine, delivered);
        }
    }
}

}  // TEST_SUITE

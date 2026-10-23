// Golden tests: hand-checked workloads whose expected results and final
// state are stored in data/workloads/golden/. Any change in engine
// behaviour shows up as a textual diff.
#include <sstream>

#include "doctest.h"
#include "fixtures.hpp"
#include "ffcsync/invariants.hpp"
#include "ffcsync/registration_engine.hpp"
#include "ffcsync/result_io.hpp"

using namespace ffcsync;
using namespace ffcsync::test;

TEST_CASE("golden workloads match expected output") {
    for (const char* name : {"basic", "last_seat", "same_student"}) {
        CAPTURE(name);
        const auto dir = data_dir() / "workloads";
        RegistrationEngine e(catalog());
        auto results = e.run(load_requests(dir / (std::string(name) + ".csv")));

        std::ostringstream res, state;
        write_results_csv(res, results);
        write_state_csv(state, catalog(), e.state());

        CHECK(res.str() == read_text(dir / "golden" / (std::string(name) + ".results.csv")));
        CHECK(state.str() == read_text(dir / "golden" / (std::string(name) + ".final_state.csv")));
        CHECK(check_invariants(catalog(), e.state()).empty());
    }
}

TEST_CASE("the same input always produces the same output") {
    auto requests = load_requests(data_dir() / "workloads" / "basic.csv");
    RegistrationEngine a(catalog()), b(catalog());
    std::ostringstream ra, rb;
    write_results_csv(ra, a.run(requests));
    write_results_csv(rb, b.run(requests));
    CHECK(ra.str() == rb.str());
    CHECK(a.state() == b.state());
}

TEST_CASE("results and admission logs round-trip through CSV") {
    RegistrationEngine e(catalog());
    auto results = e.run(load_requests(data_dir() / "workloads" / "basic.csv"));
    const auto dir = std::filesystem::temp_directory_path() / "ffcsync_roundtrip";
    write_file(dir / "r.csv", [&](std::ostream& o) { write_results_csv(o, results); });
    write_file(dir / "a.csv", [&](std::ostream& o) { write_admission_csv(o, results); });

    auto back = read_results_csv(dir / "r.csv");
    REQUIRE(back.size() == results.size());
    for (std::size_t i = 0; i < back.size(); ++i) {
        CHECK(back[i].seq == results[i].seq);
        CHECK(back[i].reason == results[i].reason);
        CHECK(back[i].request.request_id == results[i].request.request_id);
    }
    auto admitted = read_admission_csv(dir / "a.csv");
    REQUIRE(admitted.size() == results.size());
    CHECK(admitted.back().seq == results.back().seq);
    std::filesystem::remove_all(dir);
}

// The invariant checker must accept engine-produced states and flag
// deliberately corrupted ones.
#include "doctest.h"
#include "fixtures.hpp"
#include "ffcsync/invariants.hpp"
#include "ffcsync/registration_engine.hpp"

using namespace ffcsync;
using namespace ffcsync::test;

namespace {

RegistrationState sample_state() {
    RegistrationEngine e(catalog());
    e.run(load_requests(data_dir() / "workloads" / "basic.csv"));
    return e.state();
}

}  // namespace

TEST_SUITE("invariants") {

TEST_CASE("empty and engine-produced states are valid") {
    CHECK(check_invariants(catalog(), RegistrationState::empty_for(catalog())).empty());
    CHECK(check_invariants(catalog(), sample_state()).empty());
}

TEST_CASE("oversold section is detected") {
    auto s = sample_state();
    s.enrolled["CSE2005-B1"] = 2;   // capacity 1
    CHECK_FALSE(check_invariants(catalog(), s).empty());
}

TEST_CASE("seat counter disagreeing with registrations is detected") {
    auto s = sample_state();
    s.students["S006"].sections.erase("CSE2005-B1");
    CHECK_FALSE(check_invariants(catalog(), s).empty());
}

TEST_CASE("wrong credit total is detected") {
    auto s = sample_state();
    s.students["S001"].credits += 10;
    CHECK_FALSE(check_invariants(catalog(), s).empty());
}

TEST_CASE("wrong bucket total is detected") {
    auto s = sample_state();
    s.students["S001"].bucket_credits["PC"] = 0;
    CHECK_FALSE(check_invariants(catalog(), s).empty());
}

TEST_CASE("timetable clash is detected") {
    RegistrationState s = RegistrationState::empty_for(catalog());
    // Register S004 in two sections that share slot B1, bypassing the rules.
    auto& st = s.students["S004"];
    st.sections = {"CSE2005-B1", "MAT2001-B1"};
    st.credits = 80;
    st.bucket_credits = {{"PC", 40}, {"UE", 40}};
    st.slots = {"B1", "TB1"};
    s.enrolled["CSE2005-B1"] = 1;
    s.enrolled["MAT2001-B1"] = 1;
    auto v = check_invariants(catalog(), s);
    REQUIRE(v.size() == 1);
    CHECK(v[0].find("clash") != std::string::npos);
}

}  // TEST_SUITE

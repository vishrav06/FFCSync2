// One test per registration rule, run against the sequential engine.
#include "doctest.h"
#include "fixtures.hpp"
#include "ffcsync/registration_engine.hpp"

using namespace ffcsync;
using namespace ffcsync::test;

namespace {

Reason reason_of(RegistrationEngine& e, const RegistrationRequest& r) { return e.apply(r).reason; }

}  // namespace

TEST_SUITE("rules") {

TEST_CASE("successful ADD updates seats, credits, bucket and timetable") {
    RegistrationEngine e(catalog());
    auto r = e.apply(add("S001", "CSE2005-A1"));
    CHECK(r.status == Status::Accepted);
    CHECK(r.reason == Reason::Ok);
    CHECK(r.seq == 1);
    const auto& st = e.state().students.at("S001");
    CHECK(st.sections.contains("CSE2005-A1"));
    CHECK(st.credits == 40);
    CHECK(st.bucket_credits.at("PC") == 40);
    CHECK(st.slots == std::set<Slot>{"A1", "TA1", "L31", "L32"});
    CHECK(e.state().enrolled.at("CSE2005-A1") == 1);
}

TEST_CASE("window closed rejects every request") {
    Catalog closed = catalog();
    closed.config.window_open = false;
    RegistrationEngine e(closed);
    CHECK(reason_of(e, add("S001", "CSE2005-A1")) == Reason::WindowClosed);
    CHECK(reason_of(e, drop("S001", "CSE2005-A1")) == Reason::WindowClosed);
}

TEST_CASE("unknown student and unknown section") {
    RegistrationEngine e(catalog());
    CHECK(reason_of(e, add("S999", "CSE2005-A1")) == Reason::UnknownStudent);
    CHECK(reason_of(e, add("S001", "NOPE-X1")) == Reason::UnknownSection);
    CHECK(reason_of(e, drop("S001", "NOPE-X1")) == Reason::UnknownSection);
}

TEST_CASE("prerequisite failure") {
    RegistrationEngine e(catalog());
    // S003 has completed CSE1001 but not CSE2001, the prerequisite of CSE2005.
    CHECK(reason_of(e, add("S003", "CSE2005-A1")) == Reason::PrereqNotMet);
    CHECK(reason_of(e, add("S003", "CSE2001-C1")) == Reason::Ok);
}

TEST_CASE("duplicate registration in the same section") {
    RegistrationEngine e(catalog());
    REQUIRE(reason_of(e, add("S001", "CSE2005-A1")) == Reason::Ok);
    CHECK(reason_of(e, add("S001", "CSE2005-A1")) == Reason::AlreadyRegistered);
    CHECK(e.state().enrolled.at("CSE2005-A1") == 1);
}

TEST_CASE("second section of the same course") {
    RegistrationEngine e(catalog());
    REQUIRE(reason_of(e, add("S001", "CSE2005-A1")) == Reason::Ok);
    CHECK(reason_of(e, add("S001", "CSE2005-B1")) == Reason::CourseAlreadyTaken);
}

TEST_CASE("full section") {
    RegistrationEngine e(catalog());
    REQUIRE(reason_of(e, add("S001", "CSE2005-B1")) == Reason::Ok);   // capacity 1
    CHECK(reason_of(e, add("S002", "CSE2005-B1")) == Reason::SectionFull);
    CHECK(e.state().enrolled.at("CSE2005-B1") == 1);
}

TEST_CASE("timetable clash") {
    RegistrationEngine e(catalog());
    REQUIRE(reason_of(e, add("S004", "CSE2005-B1")) == Reason::Ok);   // slots B1, TB1
    CHECK(reason_of(e, add("S004", "MAT2001-B1")) == Reason::TimetableClash);
    REQUIRE(reason_of(e, add("S007", "CSE2005-A1")) == Reason::Ok);   // slots A1, TA1, ...
    CHECK(reason_of(e, add("S007", "CSE3002-A1")) == Reason::TimetableClash);
}

TEST_CASE("credit limit") {
    RegistrationEngine e(catalog());
    for (const char* sec : {"CSE1001-F2", "CSE2001-C1", "CSE3001-D1", "CSE3002-A1", "CSE4001-E1",
                            "CSE4019-F1", "MAT2001-G1", "HUM1021-A2"}) {
        REQUIRE(reason_of(e, add("S004", sec)) == Reason::Ok);
    }
    REQUIRE(e.state().students.at("S004").credits == 270);   // exactly the maximum
    CHECK(reason_of(e, add("S004", "ENG1011-B2")) == Reason::CreditLimit);
}

TEST_CASE("bucket limit") {
    RegistrationEngine e(catalog());
    REQUIRE(reason_of(e, add("S001", "MAT2001-G1")) == Reason::Ok);   // UE 40
    REQUIRE(reason_of(e, add("S001", "HUM1021-A2")) == Reason::Ok);   // UE 60 (limit)
    CHECK(reason_of(e, add("S001", "ENG1011-B2")) == Reason::BucketLimit);
}

TEST_CASE("invalid drop") {
    RegistrationEngine e(catalog());
    CHECK(reason_of(e, drop("S001", "CSE2005-A1")) == Reason::NotRegistered);
    // DROP does not check prerequisites: S003 cannot take CSE2005 but the
    // rejection reason is still NOT_REGISTERED.
    CHECK(reason_of(e, drop("S003", "CSE2005-A1")) == Reason::NotRegistered);
}

TEST_CASE("successful drop releases the seat and restores the student state") {
    RegistrationEngine e(catalog());
    const auto before = e.state();
    REQUIRE(reason_of(e, add("S001", "CSE2005-B1")) == Reason::Ok);
    CHECK(reason_of(e, add("S002", "CSE2005-B1")) == Reason::SectionFull);
    CHECK(reason_of(e, drop("S001", "CSE2005-B1")) == Reason::Ok);
    CHECK(e.state() == before);
    CHECK(reason_of(e, add("S002", "CSE2005-B1")) == Reason::Ok);
}

TEST_CASE("sequence numbers increase and replay rejects going backwards") {
    RegistrationEngine e(catalog());
    CHECK(e.apply_with_seq(5, add("S001", "CSE2005-A1")).seq == 5);
    CHECK(e.apply(add("S002", "CSE2005-A1")).seq == 6);
    CHECK_THROWS(e.apply_with_seq(6, add("S006", "CSE2005-A1")));
}

}  // TEST_SUITE
